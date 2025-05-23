/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */



#include "common/util.h"
#include "engines/engine.h"

#include "scumm/imuse/imuse_internal.h"
#include "scumm/scumm.h"

#include "audio/midiparser.h"

namespace Scumm {

////////////////////////////////////////
//
//  Miscellaneous
//
////////////////////////////////////////

#define IMUSE_SYSEX_ID 0x7D
#define YM2612_SYSEX_ID 0x7C
#define ROLAND_SYSEX_ID 0x41
#define PERCUSSION_CHANNEL 9

extern MidiParser *MidiParser_createRO();

uint16 Player::_active_notes[128];



//////////////////////////////////////////////////
//
// IMuse Player implementation
//
//////////////////////////////////////////////////

Player::Player() :
	_midi(nullptr),
	_parser(nullptr),
	_parts(nullptr),
	_active(false),
	_scanning(false),
	_id(0),
	_priority(0),
	_volume(0),
	_pan(0),
	_transpose(0),
	_detune(0),
	_note_offset(0),
	_vol_eff(0),
	_track_index(0),
	_loop_to_beat(0),
	_loop_from_beat(0),
	_loop_counter(0),
	_loop_to_tick(0),
	_loop_from_tick(0),
	_speed(128),
	_isMT32(false),
	_isMIDI(false),
	_supportsPercussion(false),
	_se(nullptr),
	_vol_chan(0),
	_abort(false),
	_music_tick(0),
	_parserType(kParserTypeNone),
	_transitionTimer(0) {
}

Player::~Player() {
	if (_parser) {
		delete _parser;
		_parser = nullptr;
	}
}

bool Player::startSound(int sound, MidiDriver *midi) {
	byte *ptr;
	int i;

	// Not sure what the old code was doing,
	// but we'll go ahead and do a similar check.
	ptr = _se->findStartOfSound(sound);
	if (!ptr) {
		error("Player::startSound(): Couldn't find start of sound %d", sound);
		return false;
	}

	_isMT32 = _se->isMT32(sound);
	_isMIDI = _se->isMIDI(sound);
	_supportsPercussion = _se->supportsPercussion(sound);

	_parts = nullptr;
	_active = true;
	_midi = midi;
	_id = sound;

	loadStartParameters(sound);

	for (i = 0; i < ARRAYSIZE(_parameterFaders); ++i)
		_parameterFaders[i].init();
	hook_clear();

	if (start_seq_sound(sound) != 0) {
		_active = false;
		_midi = nullptr;
		return false;
	}

	debugC(DEBUG_IMUSE, "Starting music %d", sound);
	return true;
}

int Player::getMusicTimer() const {
	return _parser ? (_parser->getTick() * 2 / _parser->getPPQN()) : 0;
}

bool Player::isFadingOut() const {
	for (int i = 0; i < ARRAYSIZE(_parameterFaders); ++i) {
		const ParameterFader &p = _parameterFaders[i];
		if (p.param == ParameterFader::pfVolume &&
			_volume + p.cntdwn * p.incr + ((p.irem + p.cntdwn * p.ifrac) / p.ttime) * p.dir == 0)
				return true;
	}
	return false;
}

void Player::clear() {
	if (!_active)
		return;
	debugC(DEBUG_IMUSE, "Stopping music %d", _id);

	if (_parser) {
		_parser->unloadMusic();
		_parser->setMidiDriver(nullptr);
	}

	uninit_parts();
	_se->ImFireAllTriggers(_id);
	_active = false;
	_midi = nullptr;
	_id = 0;
	_note_offset = 0;
	_speed = _se->_newSystem ? 64 : 128;
}

void Player::hook_clear() {
	_hook.reset();
}

int Player::start_seq_sound(int sound, bool reset_vars) {
	byte *ptr;

	if (reset_vars) {
		_loop_to_beat = 1;
		_loop_from_beat = 1;
		_track_index = 0;
		_loop_counter = 0;
		_loop_to_tick = 0;
		_loop_from_tick = 0;
	}

	ptr = _se->findStartOfSound(sound);
	if (ptr == nullptr)
		return -1;

	if (!memcmp(ptr, "RO", 2)) {
		// Old style 'RO' resource
		if (_parserType != kParserTypeRO) {
			delete _parser;
			_parser = MidiParser_createRO();
			_parserType = kParserTypeRO;
		}
	} else if (!memcmp(ptr, "FORM", 4)) {
		// Humongous Games XMIDI resource
		if (_parserType != kParserTypeXMI) {
			delete _parser;
			_parser = MidiParser::createParser_XMIDI();
			_parserType = kParserTypeXMI;
		}
	} else {
		// SCUMM SMF resource
		if (_parserType != kParserTypeSMF) {
			delete _parser;
			_parser = MidiParser::createParser_SMF();
			_parserType = kParserTypeSMF;
		}
	}

	_parser->setMidiDriver(this);
	_parser->property(MidiParser::mpSmartJump, 1);
	_parser->loadMusic(ptr, 0);
	_parser->setTrack(_track_index);

	ptr = _se->findStartOfSound(sound, IMuseInternal::kMDhd);

	int defSpeed = _se->_newSystem ? 64 : 128;
	setSpeed(reset_vars ? (ptr ? (READ_BE_UINT32(&ptr[4]) && ptr[15] ? ptr[15] : defSpeed) : defSpeed) : _speed);

	return 0;
}

void Player::loadStartParameters(int sound) {
	_priority = _se->_newSystem ? 0x40 : 0x80;
	_volume = 0x7F;
	_vol_chan = 0xFFFF;
	_vol_eff = (_se->get_channel_volume(0xFFFF) << 7) >> 7;
	_pan = 0;
	_transpose = 0;
	_detune = 0;

	byte *ptr = _se->findStartOfSound(sound, IMuseInternal::kMDhd);
	uint32 size = 0;

	if (ptr) {
		ptr += 4;
		size = READ_BE_UINT32(ptr);
		ptr += 4;

		// MDhd chunks don't get used in MI1 and contain only zeroes.
		// We check for volume, priority and speed settings of zero here.
		if (size && (ptr[2] | ptr[3] | ptr[7])) {
			_priority = ptr[2];
			_volume = ptr[3];
			_pan = ptr[4];
			_transpose = ptr[5];
			_detune = ptr[6];
			setSpeed(ptr[7]);
		}
	}
}

void Player::uninit_parts() {
	assert(!_parts || _parts->_player == this);

	while (_parts)
		_parts->uninit();

	// In case another player is waiting to allocate parts
	if (_midi)
		_se->reallocateMidiChannels(_midi);
}

void Player::setSpeed(byte speed) {
	// While the old system (MI1, MI2, DOTT) uses 128 as the default,
	// making anything below slower and anything above faster, the new
	// system centers on 64. Consequently, the new system does not accept
	// values above 127, while the old one accepts anything.
	int shift = 7;

	if (_se->_newSystem) {
		shift = 6;
		if (speed > 127)
			return;
	}

	_speed = speed;
	if (_parser)
		_parser->setTimerRate(((_midi->getBaseTempo() * speed) >> shift) * _se->_tempoFactor / 100);
}

void Player::send(uint32 b) {
	byte cmd = (byte)(b & 0xF0);
	byte chan = (byte)(b & 0x0F);
	byte param1 = (byte)((b >> 8) & 0xFF);
	byte param2 = (byte)((b >> 16) & 0xFF);
	Part *part;

	switch (cmd >> 4) {
	case 0x8: // Key Off
		if (!_scanning) {
			if ((part = getPart(chan)) != nullptr)
				part->noteOff(param1);
		} else {
			_active_notes[param1] &= ~(1 << chan);
		}
		break;

	case 0x9: // Key On
		param1 += _note_offset;
		if (!_scanning) {
			if (_isMT32 && !_se->isNativeMT32())
				param2 = (((param2 * 3) >> 2) + 32) & 0x7F;
			if ((part = getPart(chan)) != nullptr)
				part->noteOn(param1, param2);
		} else {
			_active_notes[param1] |= (1 << chan);
		}
		break;

	case 0xB: // Control Change
		part = (param1 == 123 ? getActivePart(chan) : getPart(chan));
		if (!part)
			break;

		switch (param1) {
		case 0: // Bank select. Not supported
			break;
		case 1: // Modulation Wheel
			part->modulationWheel(param2);
			break;
		case 7: // Volume
			part->volume(param2);
			break;
		case 10: // Pan Position
			part->set_pan(param2 - 0x40);
			break;
		case 16: // Pitchbend Factor(non-standard)
			part->pitchBendFactor(param2);
			break;
		case 17: // GP Slider 2
			if (_se->_newSystem)
				part->set_polyphony(param2);
			else
				part->set_detune(param2 - 0x40);
			break;
		case 18: // GP Slider 3
			if (!_se->_newSystem)
				param2 -= 0x40;
			part->set_pri(param2);
			_se->reallocateMidiChannels(_midi);
			break;
		case 64: // Sustain Pedal
			part->sustain(param2 != 0);
			break;
		case 91: // Effects Level
			part->effectLevel(param2);
			break;
		case 93: // Chorus Level
			part->chorusLevel(param2);
			break;
		case 116: // XMIDI For Loop. Not supported
			// Used in the ending sequence of puttputt
			break;
		case 117: // XMIDI Next/Break. Not supported
			// Used in the ending sequence of puttputt
			break;
		case 123: // All Notes Off
			part->allNotesOff();
			break;
		default:
			error("Player::send(): Invalid control change %d", param1);
		}
		break;

	case 0xC: // Program Change
		part = getPart(chan);
		if (part) {
			if (_isMIDI) {
				if (param1 < 128)
					part->programChange(param1);
			} else {
				if (param1 < 32)
					part->load_global_instrument(param1);
			}
		}
		break;

	case 0xE: // Pitch Bend (or also volume fade for Samnmax)
		part = getPart(chan);
		if (part)
			part->pitchBend(((param2 << 7) | param1) - 0x2000);
		break;

	case 0xA: // Aftertouch
	case 0xD: // Channel Pressure
	case 0xF: // Sequence Controls
		break;

	default:
		if (!_scanning) {
			error("Player::send(): Invalid command %d", cmd);
			clear();
		}
	}
	return;
}

void Player::sysEx(const byte *p, uint16 len) {
	byte a;
	byte buf[128];
	Part *part;

	// Check SysEx manufacturer.
	a = *p++;
	--len;
	if (a != IMUSE_SYSEX_ID) {
		if (a == ROLAND_SYSEX_ID) {
			// Roland custom instrument definition.
			// There is at least one (pointless) attempt in INDY4 Amiga to send this, too.
			if ((_isMIDI && _se->_soundType != MDT_AMIGA) || _isMT32) {
				part = getPart(p[0] & 0x0F);
				if (part) {
					part->_instrument.roland(p - 1);
					if (part->clearToTransmit())
						part->_instrument.send(part->_mc);
				}
			}
		} else {
			// SysEx manufacturer 0x97 has been spotted in the
			// Monkey Island 2 AdLib music, so don't make this a
			// fatal error. See bug #2595.
			// The Macintosh version of Monkey Island 2 simply
			// ignores these SysEx events too.
			if (a == 0)
				warning("Unknown SysEx manufacturer 0x00 0x%02X 0x%02X", p[0], p[1]);
			else
				warning("Unknown SysEx manufacturer 0x%02X", (int)a);
		}
		return;
	}
	--len;

	// Too big?
	if (len >= sizeof(buf))
		return;

	if (!_scanning) {
		for (a = 0; a < len + 1 && a < 19; ++a) {
			snprintf((char *)&buf[a * 3], 3 * sizeof(char) + 1, " %02X", (int)p[a]);
		}
		if (a < len + 1 && (a * 3 < sizeof(buf) - 2)) {
			if (a * 3 + 2 < int(sizeof(buf)))
				buf[a * 3] = buf[a * 3 + 1] = buf[a * 3 + 2] = '.';
			else
				warning("Player::sysEx(): Message too long (truncated)");
			++a;
		}
		if (a * 3 < sizeof(buf))
			buf[a * 3] = '\0';
		else
			warning("Player::sysEx(): Message too long (truncated)");
		debugC(DEBUG_IMUSE, "[%02d] SysEx:%s", _id, buf);
	}

	if (_se->_sysex)
		(*_se->_sysex)(this, p, len);
}

uint16 Player::sysExNoDelay(const byte *msg, uint16 length) {
	sysEx(msg, length);

	// The reason for adding this delay was the music track in the MI2 start scene (on the bridge, with Largo) when
	// played on real hardware (in my case a Roland CM32L). The track starts with several sysex messages (mostly
	// iMuse control messages, but also a Roland custom timbre sysex message). When played through the Munt emulator
	// this works totally fine, but the real hardware seems to still "choke" on the sysex data, when the actual song
	// playback has already started. This will cause a skipping of the first couple of notes, since the midi parser
	// will not wait, but strictly enforce sync on the next time stamps.
	// My tests with the dreamm emulator on that scene did sometimes show the same issue (although to a weaker extent),
	// but most of the time not. So it seems to be rather a delicate and race-condition prone matter. The original
	// parser handles the timing differently than our general purpose parser and the code execution is also expected
	// to be much slower, so that might make all the difference here. It is really a flaw of the track. The time stamps
	// after the sysex messages should have been made a bit more generous.
	// Now, I have added some delays here that I have taken from the original DOTT MT-32 driver's sysex function which
	// are supposed to handle the situation when _scanning is enabled. For non-_scanning situations there is no delay in
	// the original driver, since apparently is wasn't necessary.
	// We only need to intercept actual hardware sysex messages here. So, for the iMuse control messages, we intercept
	// just type 0, since that one leads to hardware messages. This is not a perfect solution, but it seems to work
	// as intended.

	if (_isMT32 && !_scanning && ((msg[0] == IMUSE_SYSEX_ID && msg[1] == 0) || msg[0] == ROLAND_SYSEX_ID))
		return length >= 25 ? 70 : 20;

	return 0;
}

void Player::decode_sysex_bytes(const byte *src, byte *dst, int len) {
	while (len >= 0) {
		*dst++ = ((src[0] << 4) & 0xFF) | (src[1] & 0xF);
		src += 2;
		len -= 2;
	}
}

void Player::maybe_jump(byte cmd, uint track, uint beat, uint tick) {
	// Is this the hook I'm waiting for?
	if (cmd && _hook._jump[0] != cmd)
		return;

	// Reset hook?
	if (cmd != 0 && cmd < 0x80) {
		_hook._jump[0] = _hook._jump[1];
		_hook._jump[1] = 0;
	}

	jump(track, beat, tick);
}

void Player::maybe_set_transpose(byte *data) {
	byte cmd;

	cmd = data[0];

	// Is this the hook I'm waiting for?
	if (cmd && _hook._transpose != cmd)
		return;

	// Reset hook?
	if (cmd != 0 && cmd < 0x80)
		_hook._transpose = 0;

	setTranspose(data[1], (int8)data[2]);
}

void Player::maybe_part_onoff(byte *data) {
	byte cmd, *p;
	uint chan;
	Part *part;

	cmd = data[1];
	chan = data[0];

	p = &_hook._part_onoff[chan];

	// Is this the hook I'm waiting for?
	if (cmd && *p != cmd)
		return;

	if (cmd != 0 && cmd < 0x80)
		*p = 0;

	part = getPart(chan);
	if (part)
		part->set_onoff(data[2] != 0);
}

void Player::maybe_set_volume(byte *data) {
	byte cmd;
	byte *p;
	uint chan;
	Part *part;

	cmd = data[1];
	chan = data[0];

	p = &_hook._part_volume[chan];

	// Is this the hook I'm waiting for?
	if (cmd && *p != cmd)
		return;

	// Reset hook?
	if (cmd != 0 && cmd < 0x80)
		*p = 0;

	part = getPart(chan);
	if (part)
		part->volume(data[2]);
}

void Player::maybe_set_program(byte *data) {
	byte cmd;
	byte *p;
	uint chan;
	Part *part;

	cmd = data[1];
	chan = data[0];

	// Is this the hook I'm waiting for?
	p = &_hook._part_program[chan];

	if (cmd && *p != cmd)
		return;

	if (cmd != 0 && cmd < 0x80)
		*p = 0;

	part = getPart(chan);
	if (part)
		part->programChange(data[2]);
}

void Player::maybe_set_transpose_part(byte *data) {
	byte cmd;
	byte *p;
	uint chan;

	cmd = data[1];
	chan = data[0];

	// Is this the hook I'm waiting for?
	p = &_hook._part_transpose[chan];

	if (cmd && *p != cmd)
		return;

	// Reset hook?
	if (cmd != 0 && cmd < 0x80)
		*p = 0;

	part_set_transpose(chan, data[2], (int8)data[3]);
}

int Player::setTranspose(byte relative, int b) {
	Part *part;

	if (b > 24 || b < -24 || relative > 1)
		return -1;
	if (relative)
		b = transpose_clamp(_transpose + b, -7, 7);

	_transpose = b;

	// MI2 and INDY4 use boundaries of -12/12 for MT-32 and -24/24 for AdLib and PC Speaker, DOTT uses -12/12 for everything.
	int lim = (_se->_game_id == GID_TENTACLE || _se->isNativeMT32()) ? 12 : 24;
	for (part = _parts; part; part = part->_next)
		part->set_transpose(part->_transpose, -lim, lim);

	return 0;
}

void Player::part_set_transpose(uint8 chan, byte relative, int8 b) {
	Part *part;

	if (b > 24 || b < -24)
		return;

	part = getPart(chan);
	if (!part)
		return;
	if (relative)
		b = transpose_clamp(b + part->_transpose, -7, 7);

	// MI2 and INDY4 use boundaries of -12/12 for MT-32 and -24/24 for AdLib and PC Speaker, DOTT uses -12/12 for everything.
	int lim = (_se->_game_id == GID_TENTACLE || _se->isNativeMT32()) ? 12 : 24;
	part->set_transpose(b, -lim, lim);
}

bool Player::jump(uint track, uint beat, uint tick) {
	if (!_parser)
		return false;
	if (_parser->setTrack(track))
		_track_index = track;
	if (!_parser->jumpToTick((beat - 1) * TICKS_PER_BEAT + tick))
		return false;
	turn_off_pedals();
	return true;
}

bool Player::setLoop(uint count, uint tobeat, uint totick, uint frombeat, uint fromtick) {
	if (tobeat + 1 >= frombeat)
		return false;

	if (tobeat == 0)
		tobeat = 1;

	// FIXME: Thread safety?
	_loop_counter = 0; // Because of possible interrupts
	_loop_to_beat = tobeat;
	_loop_to_tick = totick;
	_loop_from_beat = frombeat;
	_loop_from_tick = fromtick;
	_loop_counter = count;

	return true;
}

void Player::clearLoop() {
	_loop_counter = 0;
}

void Player::turn_off_pedals() {
	Part *part;

	for (part = _parts; part; part = part->_next) {
		if (part->_pedal)
			part->sustain(false);
	}
}

Part *Player::getActivePart(uint8 chan) {
	Part *part = _parts;
	while (part) {
		if (part->_chan == chan)
			return part;
		part = part->_next;
	}
	return nullptr;
}

Part *Player::getPart(uint8 chan) {
	Part *part = getActivePart(chan);
	if (part)
		return part;

	part = _se->allocate_part(_priority, _midi);
	if (!part) {
		debug(1, "No parts available");
		return nullptr;
	}

	// Insert part into front of parts list
	part->_prev = nullptr;
	part->_next = _parts;
	if (_parts)
		_parts->_prev = part;
	_parts = part;


	part->_chan = chan;
	part->setup(this);

	return part;
}

void Player::setPriority(int pri) {
	Part *part;

	_priority = pri;
	for (part = _parts; part; part = part->_next) {
		part->set_pri(part->_pri);
	}
	_se->reallocateMidiChannels(_midi);
}

void Player::setPan(int pan) {
	Part *part;

	_pan = pan;
	for (part = _parts; part; part = part->_next) {
		part->set_pan(part->_pan);
	}
}

void Player::setDetune(int detune) {
	Part *part;

	_detune = detune;
	for (part = _parts; part; part = part->_next) {
		part->set_detune(part->_detune);
	}
}

void Player::setOffsetNote(int offset) {
	_note_offset = offset;
}

int Player::scan(uint totrack, uint tobeat, uint totick) {
	if (!_active || !_parser)
		return -1;

	if (tobeat == 0)
		tobeat++;

	turn_off_parts();
	memset(_active_notes, 0, sizeof(_active_notes));
	_scanning = true;

	// If the scan involves a track switch, scan to the end of
	// the current track so that our state when starting the
	// new track is fully up to date.
	if (totrack != _track_index)
		_parser->jumpToTick((uint32)-1, true);
	_parser->setTrack(totrack);
	if (!_parser->jumpToTick((tobeat - 1) * TICKS_PER_BEAT + totick, true)) {
		_scanning = false;
		return -1;
	}

	_scanning = false;
	_se->reallocateMidiChannels(_midi);
	play_active_notes();

	if (_track_index != totrack) {
		_track_index = totrack;
		_loop_counter = 0;
	}
	return 0;
}

void Player::turn_off_parts() {
	Part *part;

	if (!_se->_dynamicChanAllocation) {
		turn_off_pedals();
		for (part = _parts; part; part = part->_next)
			part->allNotesOff();
	} else {
		for (part = _parts; part; part = part->_next)
			part->off();
		_se->reallocateMidiChannels(_midi);
	}
}

void Player::play_active_notes() {
	int i, j;
	uint mask;
	Part *part;

	for (i = 0; i < 16; ++i) {
		part = getPart(i);
		if (part) {
			mask = 1 << i;
			for (j = 0; j < 128; ++j) {
				if (_active_notes[j] & mask)
					part->noteOn(j, 80);
			}
		}
	}
}

int Player::setVolume(byte vol) {
	Part *part;

	if (vol > 127)
		return -1;

	_volume = vol;
	_vol_eff = _se->get_channel_volume(_vol_chan) * (vol + 1) >> 7;

	for (part = _parts; part; part = part->_next) {
		part->volume(part->_vol);
	}

	return 0;
}

int Player::getParam(int param, byte chan) {
	switch (param) {
	case 0:
		return (byte)_priority;
	case 1:
		return (byte)_volume;
	case 2:
		return (byte)_pan;
	case 3:
		return (byte)_transpose;
	case 4:
		return (byte)_detune;
	case 5:
		return _speed;
	case 6:
		return _track_index;
	case 7:
		return getBeatIndex();
	case 8:
		return (_parser ? _parser->getTick() % TICKS_PER_BEAT : 0); // _tick_index;
	case 9:
		return _loop_counter;
	case 10:
		return _loop_to_beat;
	case 11:
		return _loop_to_tick;
	case 12:
		return _loop_from_beat;
	case 13:
		return _loop_from_tick;
	case 14:
	case 15:
	case 16:
	case 17:
		return query_part_param(param, chan);
	case 18:
	case 19:
	case 20:
	case 21:
	case 22:
	case 23:
		return _hook.query_param(param, chan);
	default:
		return -1;
	}
}

int Player::query_part_param(int param, byte chan) {
	Part *part;

	part = _parts;
	while (part) {
		if (part->_chan == chan) {
			switch (param) {
			case 14:
				return part->_on;
			case 15:
				return part->_vol;
			case 16:
// FIXME: Need to know where this occurs...
				error("Trying to cast instrument (%d, %d) -- please tell Fingolfin", param, chan);
// In old versions of the code, this used to return part->_program.
// This was changed in revision 2.29 of imuse.cpp (where this code used
// to reside).
//				return (int)part->_instrument;
			case 17:
				return part->_transpose;
			default:
				return -1;
			}
		}
		part = part->_next;
	}
	return 129;
}

void Player::onTimer() {
	// First handle any parameter transitions
	// that are occurring.
	transitionParameters();

	// Since the volume parameter can cause
	// the player to be deactivated, check
	// to make sure we're still active.
	if (!_active || !_parser)
		return;

	uint32 target_tick = _parser->getTick();
	uint beat_index = target_tick / TICKS_PER_BEAT + 1;
	uint tick_index = target_tick % TICKS_PER_BEAT;

	if (_loop_counter && (beat_index > _loop_from_beat ||
	    (beat_index == _loop_from_beat && tick_index >= _loop_from_tick))) {
		_loop_counter--;
		jump(_track_index, _loop_to_beat, _loop_to_tick);
	}
	_parser->onTimer();
}

int Player::addParameterFader(int param, int target, int time) {
	int start;

	switch (param) {
	case ParameterFader::pfVolume:
		if (!time) {
			setVolume(target);
			return 0;
		}
		start = _volume;
		break;

	case ParameterFader::pfTranspose:
		// It's set to fade to -2400 in the tunnel of love.
		// debug(0, "parameterTransition(3) outside Tunnel of Love?");
		if (!time) {
			setDetune(target);
			return 0;
		}
		start = _detune;
		break;

	case ParameterFader::pfSpeed: // impSpeed
		start = _speed;
		break;

	case 127: {
		// FIXME? I *think* this clears all parameter faders.
		ParameterFader *ptr = &_parameterFaders[0];
		int i;
		for (i = ARRAYSIZE(_parameterFaders); i; --i, ++ptr)
			ptr->param = 0;
		return 0;
	}
	break;

	default:
		debug(0, "Player::addParameterFader(%d, %d, %d): Unknown parameter", param, target, time);
		return 0; // Should be -1, but we'll let the script think it worked.
	}

	ParameterFader *ptr = &_parameterFaders[0];
	ParameterFader *best = nullptr;
	int i;
	for (i = ARRAYSIZE(_parameterFaders); i; --i, ++ptr) {
		if (ptr->param == param) {
			best = ptr;
			break;
		} else if (!ptr->param) {
			best = ptr;
		}
	}

	if (best) {
		best->param = param;
		best->state = start;
		best->ttime = best->cntdwn = time;
		int diff = target - start;
		best->dir = (diff >= 0) ? 1 : -1;
		best->incr = diff / time;
		best->ifrac = ABS<int>(diff) % time;
		best->irem = 0;

	} else {
		debug(0, "IMuse Player %d: Out of parameter faders", _id);
		return -1;
	}

	return 0;
}

void Player::transitionParameters() {
	uint32 advance = _midi->getBaseTempo();

	_transitionTimer += advance;
	while (_transitionTimer >= 16667) {
		_transitionTimer -= 16667;

		ParameterFader *ptr = &_parameterFaders[0];
		for (int i = ARRAYSIZE(_parameterFaders); i; --i, ++ptr) {
			if (!ptr->param)
				continue;

			int32 mod = ptr->incr;
			ptr->irem += ptr->ifrac;
			if (ptr->irem >= ptr->ttime) {
				ptr->irem -= ptr->ttime;
				mod += ptr->dir;
			}
			if (!mod) {
				if (!ptr->cntdwn || !--ptr->cntdwn)
					ptr->param = 0;
				continue;
			}

			ptr->state += mod;


			switch (ptr->param) {
			case ParameterFader::pfVolume:
				// Volume
				if (ptr->state >= 0 && ptr->state <= 127) {
					setVolume((byte)ptr->state);
					if (ptr->state == 0) {
						clear();
						return;
					}
				}
				break;

			case ParameterFader::pfTranspose:
				if (ptr->state >= -9216 && ptr->state <= 9216)
					setDetune(ptr->state);
				break;

			case ParameterFader::pfSpeed:
				// Speed.
				if (ptr->state >= 0 && ptr->state <= 127) {
					setSpeed((byte)ptr->state);
				}
				break;

			default:
				ptr->param = 0;
			}

			if (!ptr->cntdwn || !--ptr->cntdwn)
				ptr->param = 0;
		}
	}
}

uint Player::getBeatIndex() {
	return (_parser ? (_parser->getTick() / TICKS_PER_BEAT + 1) : 0);
}

void Player::removePart(Part *part) {
	// Unlink
	if (part->_next)
		part->_next->_prev = part->_prev;
	if (part->_prev)
		part->_prev->_next = part->_next;
	else
		_parts = part->_next;
	part->_next = part->_prev = nullptr;
}

void Player::fixAfterLoad() {
	_midi = _se->getBestMidiDriver(_id);
	if (!_midi) {
		clear();
	} else {
		start_seq_sound(_id, false);
		setSpeed(_speed);
		if (_parser)
			_parser->jumpToTick(_music_tick); // start_seq_sound already switched tracks
		_isMT32 = _se->isMT32(_id);
		_isMIDI = _se->isMIDI(_id);
		_supportsPercussion = _se->supportsPercussion(_id);
	}
}

void Player::metaEvent(byte type, byte *msg, uint16 len) {
	if (type == 0x2F)
		clear();
}


////////////////////////////////////////
//
//  Player save/load functions
//
////////////////////////////////////////

static void syncWithSerializer(Common::Serializer &s, ParameterFader &pf) {
	s.syncAsSint16LE(pf.param, VER(17));
	if (s.isLoading() && s.getVersion() < 116) {
		int16 start, end;
		uint32 tt, ct;
		s.syncAsSint16LE(start, VER(17));
		s.syncAsSint16LE(end, VER(17));
		s.syncAsUint32LE(tt, VER(17));
		s.syncAsUint32LE(ct, VER(17));
		int32 diff = end - start;
		if (pf.param && diff && tt) {
			if (tt < 10000) {
				tt = 10000;
				ct = tt - diff;
			}
			pf.dir = diff / ABS<int>(diff);
			pf.incr = diff / (tt / 10000);
			pf.ifrac = ABS<int>(diff) % (tt / 10000);
			pf.state = (int32)start + diff * (int32)ct / (int32)tt;
		} else {
			pf.param = 0;
		}
		pf.irem = 0;
		pf.cntdwn = 0;

	} else {
		s.syncAsSByte(pf.dir, VER(116));
		s.syncAsSint16LE(pf.incr, VER(116));
		s.syncAsUint16LE(pf.ifrac, VER(116));
		s.syncAsUint16LE(pf.irem, VER(116));
		s.syncAsUint16LE(pf.ttime, VER(116));
		s.syncAsUint16LE(pf.cntdwn, VER(116));
		s.syncAsSint16LE(pf.state, VER(116));
	}
}

void Player::saveLoadWithSerializer(Common::Serializer &s) {
	if (!s.isSaving() && _parser) {
		delete _parser;
		_parser = nullptr;
		_parserType = kParserTypeNone;
	}
	_music_tick = _parser ? _parser->getTick() : 0;

	int num;
	if (s.isSaving()) {
		num = (_parts ? (_parts - _se->_parts + 1) : 0);
		s.syncAsUint16LE(num);
	} else {
		s.syncAsUint16LE(num);
		_parts = (num ? &_se->_parts[num - 1] : nullptr);
	}

	s.syncAsByte(_active, VER(8));
	s.syncAsUint16LE(_id, VER(8));
	s.syncAsByte(_priority, VER(8));
	s.syncAsByte(_volume, VER(8));
	s.syncAsSByte(_pan, VER(8));
	s.syncAsByte(_transpose, VER(8));
	s.syncAsSByte(_detune, VER(8), VER(115));
	s.syncAsSint16LE(_detune, VER(116));
	s.syncAsUint16LE(_vol_chan, VER(8));
	s.syncAsByte(_vol_eff, VER(8));
	s.syncAsByte(_speed, VER(8));
	s.skip(2, VER(8), VER(19)); // _song_index
	s.syncAsUint16LE(_track_index, VER(8));
	s.skip(2, VER(8), VER(17)); // _timer_counter
	s.syncAsUint16LE(_loop_to_beat, VER(8));
	s.syncAsUint16LE(_loop_from_beat, VER(8));
	s.syncAsUint16LE(_loop_counter, VER(8));
	s.syncAsUint16LE(_loop_to_tick, VER(8));
	s.syncAsUint16LE(_loop_from_tick, VER(8));
	s.skip(4, VER(8), VER(19)); // _tempo
	s.skip(4, VER(8), VER(17)); // _cur_pos
	s.skip(4, VER(8), VER(17)); // _next_pos
	s.skip(4, VER(8), VER(17)); // _song_offset
	s.skip(2, VER(8), VER(17)); // _tick_index
	s.skip(2, VER(8), VER(17)); // _beat_index
	s.skip(2, VER(8), VER(17)); // _ticks_per_beat
	s.syncAsUint32LE(_music_tick, VER(19));
	s.syncAsByte(_hook._jump[0], VER(8));
	s.syncAsByte(_hook._transpose, VER(8));
	s.syncBytes(_hook._part_onoff, 16, VER(8));
	s.syncBytes(_hook._part_volume, 16, VER(8));
	s.syncBytes(_hook._part_program, 16, VER(8));
	s.syncBytes(_hook._part_transpose, 16, VER(8));
	s.syncArray(_parameterFaders, ARRAYSIZE(_parameterFaders), syncWithSerializer);

	if (_se->_newSystem && s.isLoading() && s.getVersion() < VER(117) && _speed == 128)
		_speed = 64;
}

} // End of namespace Scumm
