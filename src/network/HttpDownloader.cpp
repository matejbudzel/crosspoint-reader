#include "HttpDownloader.h"

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <base64.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace {
// RX holds the response headers. 4096 fits real OPDS servers; GitHub's release
// CDN sends more and logs HTTP_HEADER "Buffer length is small", but that's
// non-fatal: the headers we read (Location, Content-Length) come first and
// survive. Smaller keeps contiguous heap free while WiFi and TLS are up. TX
// only carries our GET; the body streams in READ_CHUNK pieces.
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
// Per-socket-op timeout. Some OPDS download endpoints are slow to send headers
// (>15s) and chunked catalogs stall mid-body, so 15s killed them. 60s gives
// slow servers room. esp_http_client's timeout_ms is uint32, so unlike Arduino
// HTTPClient's uint16 setTimeout it doesn't silently truncate.
constexpr int HTTP_TIMEOUT_MS = 60000;
constexpr size_t READ_CHUNK = 512;
constexpr size_t DEBUG_PROGRESS_BYTES = 256 * 1024;
constexpr const char* HTTP_DEBUG_LOG_PATH = "/.crosspoint/http-debug.log";

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;  // returns false to abort the transfer
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  size_t total = 0;
  size_t downloaded = 0;
  size_t nextDebugProgress = DEBUG_PROGRESS_BYTES;
  bool writeFailed = false;
};

bool isRedirect(int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

void httpDebugLog(const char* fmt, ...) {
  Storage.mkdir("/.crosspoint");
  HalFile file = Storage.open(HTTP_DEBUG_LOG_PATH, O_WRITE | O_CREAT | O_APPEND);
  if (!file) {
    return;
  }

  char line[320];
  va_list args;
  va_start(args, fmt);
  const int written = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (written <= 0) {
    return;
  }

  file.write(reinterpret_cast<const uint8_t*>(line), static_cast<size_t>(std::min(written, static_cast<int>(sizeof(line) - 1))));
  file.close();
}

const char* errorName(const HttpDownloader::DownloadError err) {
  switch (err) {
    case HttpDownloader::OK:
      return "OK";
    case HttpDownloader::HTTP_ERROR:
      return "HTTP_ERROR";
    case HttpDownloader::FILE_ERROR:
      return "FILE_ERROR";
    case HttpDownloader::ABORTED:
      return "ABORTED";
  }
  return "UNKNOWN";
}

void logProgressIfNeeded(const char* mode, Sink& sink) {
  if (sink.downloaded >= sink.nextDebugProgress) {
    httpDebugLog("[%lu] %s progress downloaded=%zu total=%zu free_heap=%u\n", millis(), mode, sink.downloaded,
                 sink.total, ESP.getFreeHeap());
    while (sink.downloaded >= sink.nextDebugProgress) {
      sink.nextDebugProgress += DEBUG_PROGRESS_BYTES;
    }
  }
}

void applyCommonHeaders(esp_http_client_handle_t client, const std::string& username, const std::string& password) {
  esp_http_client_set_header(client, "User-Agent", "CrossPoint-ESP32-" CROSSPOINT_VERSION);
  esp_http_client_set_header(client, "Connection", "close");
  if (!username.empty() && !password.empty()) {
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }
}

esp_err_t fetchEventHandler(esp_http_client_event_t* evt) {
  auto* sink = static_cast<Sink*>(evt->user_data);

  switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
      httpDebugLog("[%lu] FETCH event connected\n", millis());
      return ESP_OK;
    case HTTP_EVENT_HEADER_SENT:
      httpDebugLog("[%lu] FETCH event headers_sent\n", millis());
      return ESP_OK;
    case HTTP_EVENT_ON_HEADER:
      httpDebugLog("[%lu] FETCH header %s: %s\n", millis(), evt->header_key ? evt->header_key : "",
                   evt->header_value ? evt->header_value : "");
      return ESP_OK;
    case HTTP_EVENT_ON_FINISH:
      httpDebugLog("[%lu] FETCH event finish downloaded=%zu\n", millis(), sink ? sink->downloaded : 0);
      return ESP_OK;
    case HTTP_EVENT_DISCONNECTED:
      httpDebugLog("[%lu] FETCH event disconnected downloaded=%zu\n", millis(), sink ? sink->downloaded : 0);
      return ESP_OK;
    default:
      break;
  }

  if (evt->event_id != HTTP_EVENT_ON_DATA || evt->data_len <= 0) {
    return ESP_OK;
  }
  if (!sink || !sink->write) {
    httpDebugLog("[%lu] FETCH event data without sink/write\n", millis());
    return ESP_FAIL;
  }

  if (!sink->write(static_cast<const uint8_t*>(evt->data), static_cast<size_t>(evt->data_len))) {
    sink->writeFailed = true;
    httpDebugLog("[%lu] FETCH write failed chunk=%d downloaded=%zu\n", millis(), evt->data_len, sink->downloaded);
    return ESP_FAIL;
  }

  sink->downloaded += static_cast<size_t>(evt->data_len);
  if (sink->progress) sink->progress(sink->downloaded, sink->total);
  logProgressIfNeeded("FETCH", *sink);
  return ESP_OK;
}

esp_http_client_config_t makeHttpConfig(const std::string& url) {
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.buffer_size = HTTP_RX_BUF;
  config.buffer_size_tx = HTTP_TX_BUF;
  config.timeout_ms = HTTP_TIMEOUT_MS;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  config.keep_alive_enable = false;
  return config;
}

HttpDownloader::DownloadError runFetch(const std::string& url, const std::string& username,
                                       const std::string& password, Sink& sink) {
  httpDebugLog("\n[%lu] FETCH start url=%s auth=%s free_heap=%u\n", millis(), url.c_str(),
               (!username.empty() && !password.empty()) ? "yes" : "no", ESP.getFreeHeap());
  esp_http_client_config_t config = makeHttpConfig(url);
  config.event_handler = fetchEventHandler;
  config.user_data = &sink;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    httpDebugLog("[%lu] FETCH result=%s reason=client_init_failed\n", millis(), errorName(HttpDownloader::HTTP_ERROR));
    return HttpDownloader::HTTP_ERROR;
  }

  applyCommonHeaders(client, username, password);

  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  const int64_t contentLength = esp_http_client_get_content_length(client);
  const bool writeFailed = sink.writeFailed;
  const size_t downloaded = sink.downloaded;
  esp_http_client_cleanup(client);

  httpDebugLog("[%lu] FETCH perform err=%s status=%d content_length=%lld downloaded=%zu write_failed=%s\n", millis(),
               esp_err_to_name(err), status, static_cast<long long>(contentLength), downloaded,
               writeFailed ? "yes" : "no");
  if (writeFailed) {
    httpDebugLog("[%lu] FETCH result=%s\n", millis(), errorName(HttpDownloader::FILE_ERROR));
    return HttpDownloader::FILE_ERROR;
  }
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "perform failed after %zu bytes: %s", downloaded, esp_err_to_name(err));
    httpDebugLog("[%lu] FETCH result=%s reason=perform_failed\n", millis(), errorName(HttpDownloader::HTTP_ERROR));
    return HttpDownloader::HTTP_ERROR;
  }
  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    httpDebugLog("[%lu] FETCH result=%s reason=unexpected_status\n", millis(), errorName(HttpDownloader::HTTP_ERROR));
    return HttpDownloader::HTTP_ERROR;
  }
  if (downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    httpDebugLog("[%lu] FETCH result=%s reason=no_data\n", millis(), errorName(HttpDownloader::HTTP_ERROR));
    return HttpDownloader::HTTP_ERROR;
  }

  httpDebugLog("[%lu] FETCH result=%s downloaded=%zu\n", millis(), errorName(HttpDownloader::OK), downloaded);
  return HttpDownloader::OK;
}

// Streams a GET body through sink.write in READ_CHUNK pieces. Uses the manual
// open/fetch_headers/read path rather than esp_http_client_perform(): perform()
// pushes the whole body through an event callback and reports a chunked body
// that ends early as ESP_ERR_HTTP_INCOMPLETE_DATA, whereas the read loop streams
// large/slow files and surfaces a short read directly.
HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     Sink& sink) {
  httpDebugLog("\n[%lu] GET start url=%s auth=%s free_heap=%u\n", millis(), url.c_str(),
               (!username.empty() && !password.empty()) ? "yes" : "no", ESP.getFreeHeap());
  // Verify HTTPS against the bundled CA roots. This build has esp-tls
  // CONFIG_ESP_TLS_INSECURE off, so an unverified TLS handshake can't be set
  // up at all; the model is public servers over verified https and local
  // servers over plain http (esp_http_client picks the transport from the URL
  // scheme, so http:// needs no cert config). The prior setInsecure() worked
  // only because Arduino's ssl_client drives mbedtls directly.
  esp_http_client_config_t config = makeHttpConfig(url);

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "client init failed");
    httpDebugLog("[%lu] GET result=%s reason=client_init_failed\n", millis(), errorName(HttpDownloader::HTTP_ERROR));
    return HttpDownloader::HTTP_ERROR;
  }

  applyCommonHeaders(client, username, password);

  // open()/read() does not auto-follow redirects (only perform() does), so step
  // 30x responses manually. OPDS download endpoints and the GitHub release CDN
  // both redirect.
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "open failed: %s", esp_err_to_name(err));
    httpDebugLog("[%lu] GET open err=%s\n", millis(), esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }
  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  bool chunked = esp_http_client_is_chunked_response(client);
  httpDebugLog("[%lu] GET headers status=%d content_length=%lld\n", millis(), status,
               static_cast<long long>(contentLength));
  for (int hop = 0; isRedirect(status) && hop < 5; ++hop) {
    httpDebugLog("[%lu] GET redirect hop=%d status=%d\n", millis(), hop + 1, status);
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "redirect open failed: %s", esp_err_to_name(err));
      httpDebugLog("[%lu] GET redirect_open err=%s\n", millis(), esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    chunked = esp_http_client_is_chunked_response(client);
    httpDebugLog("[%lu] GET redirect headers status=%d content_length=%lld chunked=%s\n", millis(), status,
                 static_cast<long long>(contentLength), chunked ? "yes" : "no");
  }
  httpDebugLog("[%lu] GET response status=%d content_length=%lld chunked=%s\n", millis(), status,
               static_cast<long long>(contentLength), chunked ? "yes" : "no");

  if (status != 200) {
    LOG_ERR("HTTP", "unexpected status: %d", status);
    httpDebugLog("[%lu] GET result=%s reason=unexpected_status status=%d\n", millis(),
                 errorName(HttpDownloader::HTTP_ERROR), status);
    esp_http_client_cleanup(client);
    return HttpDownloader::HTTP_ERROR;
  }

  // fetch_headers returns 0 for a chunked response (no Content-Length); leave
  // total at 0 so progress stays silent and the size check is skipped.
  sink.total = contentLength > 0 ? static_cast<size_t>(contentLength) : 0;

  char buf[READ_CHUNK];

  while (true) {
    if (sink.cancelFlag && *sink.cancelFlag) {
      httpDebugLog("[%lu] GET result=%s reason=cancel downloaded=%zu total=%zu\n", millis(),
                   errorName(HttpDownloader::ABORTED), sink.downloaded, sink.total);
      esp_http_client_cleanup(client);
      return HttpDownloader::ABORTED;
    }
    if (sink.total > 0 && sink.downloaded >= sink.total) {
      break;
    }
    const int read = esp_http_client_read(client, buf, READ_CHUNK);
    if (read < 0) {
      LOG_ERR("HTTP", "read error after %zu bytes", sink.downloaded);
      httpDebugLog("[%lu] GET read_error downloaded=%zu total=%zu\n", millis(), sink.downloaded, sink.total);
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (read == 0) {
      httpDebugLog("[%lu] GET read_zero downloaded=%zu total=%zu complete=%s\n", millis(), sink.downloaded, sink.total,
                   esp_http_client_is_complete_data_received(client) ? "yes" : "no");
      break;  // all data received, or no more data before timeout
    }
    if (!sink.write(reinterpret_cast<const uint8_t*>(buf), read)) {
      httpDebugLog("[%lu] GET result=%s reason=write_failed chunk=%d downloaded=%zu\n", millis(),
                   errorName(HttpDownloader::FILE_ERROR), read, sink.downloaded);
      esp_http_client_cleanup(client);
      return HttpDownloader::FILE_ERROR;
    }
    sink.downloaded += read;
    if (sink.progress) sink.progress(sink.downloaded, sink.total);
    logProgressIfNeeded("GET", sink);
  }

  const bool complete = sink.total > 0 ? sink.downloaded >= sink.total
                        : chunked       ? sink.downloaded > 0
                                        : esp_http_client_is_complete_data_received(client);
  esp_http_client_cleanup(client);
  if (!complete) {
    LOG_ERR("HTTP", "incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
    httpDebugLog("[%lu] GET result=%s reason=incomplete downloaded=%zu total=%zu\n", millis(),
                 errorName(HttpDownloader::HTTP_ERROR), sink.downloaded, sink.total);
    return HttpDownloader::HTTP_ERROR;
  }
  httpDebugLog("[%lu] GET result=%s downloaded=%zu total=%zu\n", millis(), errorName(HttpDownloader::OK),
               sink.downloaded, sink.total);
  return HttpDownloader::OK;
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; };
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  outContent.clear();  // start clean; the sink appends, so don't carry prior content
  Sink sink;
  sink.write = [&outContent](const uint8_t* data, size_t len) {
    outContent.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  return runGet(url, username, password, sink) == OK;
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  LOG_DBG("HTTP", "Fetching: %s", url.c_str());
  Sink sink;
  sink.write = onData;
  return runGet(url, username, password, sink) == OK;
}

HttpDownloader::ProbeResult HttpDownloader::probeContentLength(const std::string& url, const std::string& username,
                                                               const std::string& password) {
  LOG_DBG("HTTP", "Probing: %s", url.c_str());
  ProbeResult result;

  esp_http_client_config_t config = makeHttpConfig(url);
  httpDebugLog("\n[%lu] PROBE start url=%s auth=%s free_heap=%u\n", millis(), url.c_str(),
               (!username.empty() && !password.empty()) ? "yes" : "no", ESP.getFreeHeap());
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    LOG_ERR("HTTP", "probe client init failed");
    httpDebugLog("[%lu] PROBE result=%s reason=client_init_failed\n", millis(), errorName(HTTP_ERROR));
    return result;
  }

  applyCommonHeaders(client, username, password);
  esp_http_client_set_method(client, HTTP_METHOD_HEAD);

  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    LOG_ERR("HTTP", "probe open failed: %s", esp_err_to_name(err));
    httpDebugLog("[%lu] PROBE open err=%s\n", millis(), esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return result;
  }

  int64_t contentLength = esp_http_client_fetch_headers(client);
  int status = esp_http_client_get_status_code(client);
  httpDebugLog("[%lu] PROBE headers status=%d content_length=%lld\n", millis(), status,
               static_cast<long long>(contentLength));
  for (int hop = 0; isRedirect(status) && hop < 5; ++hop) {
    httpDebugLog("[%lu] PROBE redirect hop=%d status=%d\n", millis(), hop + 1, status);
    if (esp_http_client_set_redirection(client) != ESP_OK) break;
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "probe redirect open failed: %s", esp_err_to_name(err));
      httpDebugLog("[%lu] PROBE redirect_open err=%s\n", millis(), esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return result;
    }
    contentLength = esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    httpDebugLog("[%lu] PROBE redirect headers status=%d content_length=%lld\n", millis(), status,
                 static_cast<long long>(contentLength));
  }

  result.httpStatus = status;
  result.result = (status >= 200 && status < 400) ? OK : HTTP_ERROR;
  result.hasContentLength = contentLength > 0;
  result.contentLength = result.hasContentLength ? static_cast<size_t>(contentLength) : 0;
  esp_http_client_cleanup(client);
  httpDebugLog("[%lu] PROBE result=%s status=%d has_length=%s length=%zu\n", millis(), errorName(result.result),
               result.httpStatus, result.hasContentLength ? "yes" : "no", result.contentLength);
  return result;
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password) {
  LOG_DBG("HTTP", "Downloading: %s -> %s", url.c_str(), destPath.c_str());
  httpDebugLog("\n[%lu] DOWNLOAD start url=%s dest=%s free_heap=%u\n", millis(), url.c_str(), destPath.c_str(),
               ESP.getFreeHeap());

  if (Storage.exists(destPath.c_str())) {
    httpDebugLog("[%lu] DOWNLOAD removing_existing dest=%s\n", millis(), destPath.c_str());
    Storage.remove(destPath.c_str());
  }
  HalFile file;
  if (!Storage.openFileForWrite("HTTP", destPath.c_str(), file)) {
    LOG_ERR("HTTP", "Failed to open file for writing");
    httpDebugLog("[%lu] DOWNLOAD result=%s reason=open_file_failed dest=%s\n", millis(), errorName(FILE_ERROR),
                 destPath.c_str());
    return FILE_ERROR;
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.write = [&file](const uint8_t* data, size_t len) { return file.write(data, len) == len; };

  const DownloadError result = runGet(url, username, password, sink);
  // Close before any remove() on the same path; DESTRUCTOR_CLOSES_FILE would
  // otherwise close only after the remove.
  file.close();

  if (result != OK) {
    Storage.remove(destPath.c_str());
    httpDebugLog("[%lu] DOWNLOAD result=%s removed_partial=yes downloaded=%zu\n", millis(), errorName(result),
                 sink.downloaded);
    return result;
  }
  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "no data received");
    Storage.remove(destPath.c_str());
    httpDebugLog("[%lu] DOWNLOAD result=%s reason=no_data removed_partial=yes\n", millis(), errorName(HTTP_ERROR));
    return HTTP_ERROR;
  }
  LOG_DBG("HTTP", "Downloaded %zu bytes", sink.downloaded);
  httpDebugLog("[%lu] DOWNLOAD result=%s dest=%s downloaded=%zu\n", millis(), errorName(OK), destPath.c_str(),
               sink.downloaded);
  return OK;
}
