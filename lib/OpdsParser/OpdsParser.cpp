#include "OpdsParser.h"

#include <Logging.h>
#include <XmlParserUtils.h>

#include <cstdlib>
#include <cctype>
#include <cstring>
#include <utility>

namespace {

bool asciiEqualsIgnoreCase(const char* left, const char* right) {
  if (!left || !right) return false;
  while (*left && *right) {
    if (std::tolower(static_cast<unsigned char>(*left)) != std::tolower(static_cast<unsigned char>(*right))) {
      return false;
    }
    left++;
    right++;
  }
  return *left == '\0' && *right == '\0';
}

bool hasSuffixIgnoreCase(const char* value, const char* suffix) {
  if (!value || !suffix) return false;
  const size_t valueLen = strlen(value);
  const size_t suffixLen = strlen(suffix);
  if (valueLen < suffixLen) return false;
  return asciiEqualsIgnoreCase(value + valueLen - suffixLen, suffix);
}

bool mediaTypeEquals(const char* value, const char* mediaType) {
  if (!value || !mediaType) return false;
  while (*value == ' ' || *value == '\t') value++;

  const size_t mediaTypeLen = strlen(mediaType);
  for (size_t i = 0; i < mediaTypeLen; i++) {
    if (std::tolower(static_cast<unsigned char>(value[i])) !=
        std::tolower(static_cast<unsigned char>(mediaType[i]))) {
      return false;
    }
  }

  const char next = value[mediaTypeLen];
  return next == '\0' || next == ';' || next == ' ' || next == '\t';
}

const char* acquisitionExtensionFor(const char* type, const char* href) {
  if (type) {
    if (mediaTypeEquals(type, "application/epub+zip")) return ".epub";
    if (mediaTypeEquals(type, "application/x-xteink-xtc") ||
        mediaTypeEquals(type, "application/x-crosspoint-xtc")) {
      return ".xtc";
    }
    if (mediaTypeEquals(type, "application/x-xteink-xtch") ||
        mediaTypeEquals(type, "application/x-crosspoint-xtch")) {
      return ".xtch";
    }
  }

  if (type && !mediaTypeEquals(type, "application/octet-stream")) {
    return nullptr;
  }
  if (hasSuffixIgnoreCase(href, ".epub")) return ".epub";
  if (hasSuffixIgnoreCase(href, ".xtc")) return ".xtc";
  if (hasSuffixIgnoreCase(href, ".xtch")) return ".xtch";
  return nullptr;
}

int acquisitionPriority(const char* extension) {
  if (asciiEqualsIgnoreCase(extension, ".xtc") || asciiEqualsIgnoreCase(extension, ".xtch")) return 3;
  if (asciiEqualsIgnoreCase(extension, ".epub")) return 2;
  return 0;
}

}  // namespace

OpdsParser::OpdsParser() {
  parser = XML_ParserCreate(nullptr);
  if (!parser) {
    errorOccured = true;
    LOG_DBG("OPDS", "Couldn't allocate memory for parser");
    return;
  }
  // Allocate the common catalog size before HTTPS/TLS setup consumes much of
  // the heap. Further vector growth moves existing entries without copying
  // their string buffers.
  entries.reserve(32);
}

OpdsParser::~OpdsParser() { destroyXmlParser(parser); }

size_t OpdsParser::write(uint8_t c) { return write(&c, 1); }

size_t OpdsParser::write(const uint8_t* xmlData, const size_t length) {
  if (errorOccured || !parser) return length;

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  const char* currentPos = reinterpret_cast<const char*>(xmlData);
  size_t remaining = length;
  constexpr size_t chunkSize = 1024;

  while (remaining > 0) {
    const size_t toRead = remaining < chunkSize ? remaining : chunkSize;
    void* const buf = XML_GetBuffer(parser, toRead);
    if (!buf) {
      errorOccured = true;
      LOG_DBG("OPDS", "Couldn't allocate memory for buffer");
      destroyXmlParser(parser);
      return length;
    }

    memcpy(buf, currentPos, toRead);

    if (XML_ParseBuffer(parser, static_cast<int>(toRead), 0) == XML_STATUS_ERROR) {
      errorOccured = true;
      LOG_DBG("OPDS", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      destroyXmlParser(parser);
      return length;
    }
    currentPos += toRead;
    remaining -= toRead;
  }
  return length;
}

void OpdsParser::flush() {
  if (!parser) return;
  if (XML_Parse(parser, nullptr, 0, XML_TRUE) != XML_STATUS_OK) {
    errorOccured = true;
    destroyXmlParser(parser);
  }
}

bool OpdsParser::error() const { return errorOccured; }

void OpdsParser::clear() {
  entries.clear();
  searchTemplate.clear();
  nextPageUrl.clear();
  prevPageUrl.clear();
  currentEntry = OpdsEntry{};
  currentText.clear();
  inEntry = inTitle = inAuthor = inAuthorName = inId = inUpdated = false;
}

std::vector<OpdsEntry> OpdsParser::getBooks() const {
  std::vector<OpdsEntry> books;
  for (const auto& entry : entries) {
    if (entry.type == OpdsEntryType::BOOK) books.push_back(entry);
  }
  return books;
}

const char* OpdsParser::findAttribute(const XML_Char** atts, const char* name) {
  for (int i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1];
  }
  return nullptr;
}

void XMLCALL OpdsParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "link") == 0 || strstr(name, ":link") != nullptr) {
    const char* href = findAttribute(atts, "href");
    if (href) {
      const char* rel = findAttribute(atts, "rel");
      const char* type = findAttribute(atts, "type");

      if (rel && strcmp(rel, "search") == 0) {
        std::string sHref(href);
        if (sHref.find("{searchTerms}") != std::string::npos) {
          self->searchTemplate = sHref;
        }
      } else if (rel && strcmp(rel, "next") == 0 && !self->inEntry) {
        self->nextPageUrl = href;
      } else if (rel && strcmp(rel, "previous") == 0 && !self->inEntry) {
        self->prevPageUrl = href;
      }

      if (self->inEntry) {
        const char* acquisitionExtension = acquisitionExtensionFor(type, href);
        if (rel && strstr(rel, "opds-spec.org/acquisition") != nullptr && acquisitionExtension) {
          // Prefer pre-rendered XTC/XTCH links over EPUB when a feed exposes
          // multiple acquisition formats for one entry.
          const int newPriority = acquisitionPriority(acquisitionExtension);
          const int currentPriority = acquisitionPriority(self->currentEntry.fileExtension.c_str());
          if (self->currentEntry.type != OpdsEntryType::BOOK || newPriority > currentPriority) {
            self->currentEntry.type = OpdsEntryType::BOOK;
            self->currentEntry.href = href;
            self->currentEntry.fileExtension = acquisitionExtension;
            const char* length = findAttribute(atts, "length");
            if (length) {
              char* end = nullptr;
              const unsigned long parsed = strtoul(length, &end, 10);
              if (end && *end == '\0') {
                self->currentEntry.acquisitionSize = parsed;
                self->currentEntry.hasAcquisitionSize = true;
              } else {
                self->currentEntry.acquisitionSize = 0;
                self->currentEntry.hasAcquisitionSize = false;
              }
            } else {
              self->currentEntry.acquisitionSize = 0;
              self->currentEntry.hasAcquisitionSize = false;
            }
          }
        } else if (type && strstr(type, "application/atom+xml") != nullptr) {
          if (self->currentEntry.type != OpdsEntryType::BOOK) {
            self->currentEntry.type = OpdsEntryType::NAVIGATION;
            self->currentEntry.href = href;
          }
        }
      }
    }
  }

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    self->inEntry = true;
    self->currentEntry = OpdsEntry{};
    return;
  }

  if (!self->inEntry) return;

  if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
    self->inTitle = true;
    self->currentText.clear();
  } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
    self->inAuthor = true;
  } else if (self->inAuthor && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
    self->inAuthorName = true;
    self->currentText.clear();
  } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
    self->inId = true;
    self->currentText.clear();
  } else if (strcmp(name, "updated") == 0 || strstr(name, ":updated") != nullptr) {
    self->inUpdated = true;
    self->currentText.clear();
  }
}

void XMLCALL OpdsParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<OpdsParser*>(userData);

  if (strcmp(name, "entry") == 0 || strstr(name, ":entry") != nullptr) {
    if (!self->currentEntry.title.empty() && !self->currentEntry.href.empty()) {
      self->entries.push_back(std::move(self->currentEntry));
    }
    self->inEntry = false;
  } else if (self->inEntry) {
    if (strcmp(name, "title") == 0 || strstr(name, ":title") != nullptr) {
      if (self->inTitle) self->currentEntry.title = std::move(self->currentText);
      self->inTitle = false;
    } else if (strcmp(name, "author") == 0 || strstr(name, ":author") != nullptr) {
      self->inAuthor = false;
    } else if (self->inAuthorName && (strcmp(name, "name") == 0 || strstr(name, ":name") != nullptr)) {
      self->currentEntry.author = std::move(self->currentText);
      self->inAuthorName = false;
    } else if (strcmp(name, "id") == 0 || strstr(name, ":id") != nullptr) {
      if (self->inId) self->currentEntry.id = std::move(self->currentText);
      self->inId = false;
    } else if (strcmp(name, "updated") == 0 || strstr(name, ":updated") != nullptr) {
      if (self->inUpdated) self->currentEntry.updated = std::move(self->currentText);
      self->inUpdated = false;
    }
  }
}

void XMLCALL OpdsParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<OpdsParser*>(userData);
  if (self->inTitle || self->inAuthorName || self->inId || self->inUpdated) {
    self->currentText.append(s, len);
  }
}
