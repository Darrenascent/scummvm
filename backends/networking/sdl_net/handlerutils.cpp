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

#include "backends/networking/sdl_net/handlerutils.h"
#include "backends/networking/sdl_net/localwebserver.h"
#include "backends/saves/default/default-saves.h"
#include "common/archive.h"
#include "common/config-manager.h"
#include "common/file.h"
#include "common/memstream.h"
#include "common/translation.h"
#include "common/compression/unzip.h"

namespace Networking {

#define ARCHIVE_NAME "wwwroot.zip"

#define INDEX_PAGE_NAME ".index.html"

Common::Archive *HandlerUtils::getZipArchive() {
	// first search in themepath
	if (ConfMan.hasKey("themepath")) {
		const Common::FSNode &node = Common::FSNode(ConfMan.getPath("themepath"));
		if (node.exists() && node.isReadable() && node.isDirectory()) {
			Common::FSNode fileNode = node.getChild(ARCHIVE_NAME);
			if (fileNode.exists() && fileNode.isReadable() && !fileNode.isDirectory()) {
				Common::SeekableReadStream *const stream = fileNode.createReadStream();
				Common::Archive *zipArchive = Common::makeZipArchive(stream);
				if (zipArchive)
					return zipArchive;
			}
		}
	}

	// then use SearchMan to find it
	Common::ArchiveMemberList fileList;
	SearchMan.listMatchingMembers(fileList, ARCHIVE_NAME);
	for (auto &m : fileList) {
		Common::SeekableReadStream *const stream = m->createReadStream();
		Common::Archive *zipArchive = Common::makeZipArchive(stream);
		if (zipArchive)
			return zipArchive;
	}

	return nullptr;
}

Common::ArchiveMemberList HandlerUtils::listArchive() {
	Common::ArchiveMemberList resultList;
	Common::Archive *zipArchive = getZipArchive();
	if (zipArchive) {
		zipArchive->listMembers(resultList);
		delete zipArchive;
	}
	return resultList;
}

Common::SeekableReadStream *HandlerUtils::getArchiveFile(const Common::String &name) {
	Common::SeekableReadStream *result = nullptr;
	Common::Archive *zipArchive = getZipArchive();
	if (zipArchive) {
		const Common::ArchiveMemberPtr ptr = zipArchive->getMember(Common::Path(name, '/'));
		if (ptr.get() == nullptr)
			return nullptr;
		result = ptr->createReadStream();
		delete zipArchive;
	}
	return result;
}

Common::String HandlerUtils::readEverythingFromStream(Common::SeekableReadStream *const stream) {
	Common::String result;
	char buf[1024];
	uint32 readBytes;
	while (!stream->eos()) {
		readBytes = stream->read(buf, 1024);
		result += Common::String(buf, readBytes);
	}
	return result;
}

Common::SeekableReadStream *HandlerUtils::makeResponseStreamFromString(const Common::String &response) {
	byte *data = new byte[response.size()];
	memcpy(data, response.c_str(), response.size());
	return new Common::MemoryReadStream(data, response.size(), DisposeAfterUse::YES);
}

Common::String HandlerUtils::normalizePath(const Common::String &path) {
	Common::String normalized;
	bool slash = false;
	for (uint32 i = 0; i < path.size(); ++i) {
		char c = path[i];
		if (c == '\\' || c == '/') {
			slash = true;
			continue;
		}

		if (slash) {
			normalized += '/';
			slash = false;
		}

		if ('A' <= c && c <= 'Z') {
			normalized += c - 'A' + 'a';
		} else {
			normalized += c;
		}
	}
	if (slash) normalized += '/';
	return normalized;
}

bool HandlerUtils::hasForbiddenCombinations(const Common::String &path) {
	return (path.contains("/../") || path.contains("\\..\\") || path.contains("\\../") || path.contains("/..\\"));
}

bool HandlerUtils::isBlacklisted(const Common::Path &path) {
	const char *blacklist[] = {
		"/etc",
		"/bin",
		"c:/windows" // just saying: I know guys who install windows on another drives
	};

	// normalize path
	Common::Path normalized = path.normalize();

	uint32 size = sizeof(blacklist) / sizeof(const char *);
	for (uint32 i = 0; i < size; ++i)
		if (normalized.isRelativeTo(Common::Path(blacklist[i], '/')))
			return true;

	return false;
}

bool HandlerUtils::hasPermittedPrefix(const Common::Path &path) {
	// normalize path
	Common::Path normalized = path.normalize();

	// prefix for /root/
	Common::Path prefix;
	if (ConfMan.hasKey("rootpath", "cloud")) {
		prefix = ConfMan.getPath("rootpath", "cloud").normalize();
		if (normalized.isRelativeTo(prefix))
			return true;
	}

	// prefix for /saves/
#ifdef USE_LIBCURL
	DefaultSaveFileManager *manager = dynamic_cast<DefaultSaveFileManager *>(g_system->getSavefileManager());
	prefix = (manager ? manager->concatWithSavesPath("") : ConfMan.getPath("savepath"));
#else
	prefix = ConfMan.getPath("savepath");
#endif
	prefix = prefix.normalize();
	return normalized.isRelativeTo(prefix);
}

bool HandlerUtils::permittedPath(const Common::Path &path) {
	return hasPermittedPrefix(path) && !isBlacklisted(path);
}

void HandlerUtils::setMessageHandler(Client &client, const Common::String &message, const Common::String &redirectTo) {
	Common::String response = "<html><head><title>ScummVM</title><meta charset=\"utf-8\"/></head><body>{message}</body></html>";

	// load stylish response page from the archive
	Common::SeekableReadStream *const stream = getArchiveFile(INDEX_PAGE_NAME);
	if (stream)
		response = readEverythingFromStream(stream);

	replace(response, "{message}", message);
	if (redirectTo.empty())
		LocalWebserver::setClientGetHandler(client, response);
	else
		LocalWebserver::setClientRedirectHandler(client, response, redirectTo);
}

void HandlerUtils::setFilesManagerErrorMessageHandler(Client &client, const Common::String &message, const Common::String &redirectTo) {
	setMessageHandler(
		client,
		Common::String::format(
			"%s<br/><a href=\"files%s?path=%s\">%s</a>",
			message.c_str(),
			client.queryParameter("ajax") == "true" ? "AJAX" : "",
			"%2F", //that's encoded "/"
			Common::convertFromU32String(_("Back to the files manager")).c_str()
		),
		redirectTo
	);
}

} // End of namespace Networking
