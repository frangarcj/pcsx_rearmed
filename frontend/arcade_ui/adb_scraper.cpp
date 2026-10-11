#include "adb_scraper.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <map>

#include <curl/curl.h>
#include <formats/rjson.h>

namespace {

const char kQuery[] = "https://adb.arcadeitalia.net/service_scraper.php?ajax=query_mame&game_name=";
/* The service asks for URLs of at most 800 characters. */
const size_t kMaxUrl = 800;
const char kUserAgent[] = "PCSX-ReArcade (PS Vita arcade emulator; https://github.com/frangarcj/pcsx_rearmed)";

pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_t s_thread;
bool s_thread_started;
AdbProgress s_progress;

struct Job {
	std::string data_root, ca_file;
	std::vector<std::string> sets;
};
Job s_job;

void set_progress(const std::string *current, int sets_done, int images, const std::string *error)
{
	pthread_mutex_lock(&s_lock);
	if (current) s_progress.current = *current;
	if (sets_done >= 0) s_progress.sets_done = sets_done;
	if (images >= 0) s_progress.images = images;
	if (error) s_progress.error = *error;
	pthread_mutex_unlock(&s_lock);
}

bool cancelled()
{
	pthread_mutex_lock(&s_lock);
	const bool value = s_progress.cancelled;
	pthread_mutex_unlock(&s_lock);
	return value;
}

size_t append(char *data, size_t size, size_t count, void *opaque)
{
	((std::string *)opaque)->append(data, size * count);
	return size * count;
}

size_t write_file(char *data, size_t size, size_t count, void *opaque)
{
	return fwrite(data, size, count, (FILE *)opaque) * size;
}

void prepare(CURL *curl, const std::string &url, const std::string &ca_file)
{
	curl_easy_reset(curl);
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	if (!ca_file.empty())
		curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file.c_str());
}

bool fetch_text(CURL *curl, const std::string &url, const std::string &ca_file, std::string *out, std::string *error)
{
	prepare(curl, url, ca_file);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
	const CURLcode code = curl_easy_perform(curl);
	if (code != CURLE_OK)
		*error = curl_easy_strerror(code);
	return code == CURLE_OK;
}

/* Downloads to path.part, then renames: no half files on a failure. */
bool fetch_file(CURL *curl, const std::string &url, const std::string &ca_file, const std::string &path,
	std::string *error)
{
	const std::string part = path + ".part";
	FILE *f = fopen(part.c_str(), "wb");
	if (!f) {
		*error = "cannot write " + part;
		return false;
	}
	prepare(curl, url, ca_file);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
	const CURLcode code = curl_easy_perform(curl);
	fclose(f);
	if (code != CURLE_OK) {
		*error = curl_easy_strerror(code);
		remove(part.c_str());
		return false;
	}
	remove(path.c_str());
	return rename(part.c_str(), path.c_str()) == 0;
}

bool exists(const std::string &path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

/* One romset of the reply: every field is a string or a number. */
typedef std::map<std::string, std::string> Record;

bool parse_reply(const std::string &text, std::vector<Record> *records)
{
	rjson_t *json = rjson_open_buffer(text.data(), text.size());
	int depth = 0;
	bool in_result = false, expect_key = true;
	std::string key;
	Record record;
	if (!json)
		return false;
	for (;;) {
		const enum rjson_type type = rjson_next(json);
		if (type == RJSON_DONE || type == RJSON_ERROR) {
			rjson_free(json);
			return type == RJSON_DONE;
		}
		switch (type) {
		case RJSON_OBJECT:
		case RJSON_ARRAY:
			depth++;
			if (depth == 3 && in_result && type == RJSON_OBJECT) {
				record.clear();
				expect_key = true;
			}
			break;
		case RJSON_OBJECT_END:
		case RJSON_ARRAY_END:
			if (depth == 3 && in_result && type == RJSON_OBJECT_END)
				records->push_back(record);
			if (depth == 2)
				in_result = false;
			depth--;
			/* A nested value inside a record ends: a key comes next. */
			if (depth == 3)
				expect_key = true;
			break;
		default: {
			size_t length = 0;
			const char *value = type == RJSON_STRING || type == RJSON_NUMBER ? rjson_get_string(json, &length) : "";
			if (depth == 1) {
				/* Top level: "release" and "result". */
				if (expect_key) {
					key.assign(value, length);
					in_result = key == "result";
				}
				expect_key = !expect_key;
			} else if (depth == 3 && in_result) {
				if (expect_key)
					key.assign(value, length);
				else
					record[key].assign(value, length);
				expect_key = !expect_key;
			}
			break;
		}
		}
		/* A key of the top object resets the alternation when its value
		 * (the result array) ends. */
		if (depth == 1 && (type == RJSON_ARRAY_END || type == RJSON_OBJECT_END))
			expect_key = true;
	}
}

std::string escape_json(const std::string &value)
{
	std::string out;
	for (char c : value) {
		if (c == '"' || c == '\\') { out += '\\'; out += c; }
		else if ((unsigned char)c < 0x20) { char hex[8]; snprintf(hex, sizeof(hex), "\\u%04x", c); out += hex; }
		else out += c;
	}
	return out;
}

/* The control and credit data the selector uses, for <data>/adb/<set>.json. */
void save_record(const std::string &path, const Record &record)
{
	static const char *const fields[] = { "game_name", "title", "manufacturer", "year", "genre", "players",
		"nplayers", "input_controls", "input_buttons", "buttons_colors", "screen_orientation" };
	FILE *f = fopen(path.c_str(), "w");
	if (!f)
		return;
	fprintf(f, "{\n  \"source\": \"%s\"", escape_json(ADB_CREDIT).c_str());
	for (const char *field : fields) {
		auto it = record.find(field);
		if (it != record.end())
			fprintf(f, ",\n  \"%s\": \"%s\"", field, escape_json(it->second).c_str());
	}
	fprintf(f, "\n}\n");
	fclose(f);
}

void *run(void *)
{
	CURL *curl = curl_easy_init();
	const std::string &root = s_job.data_root;
	int done = 0, images = 0;
	std::string error;
	static const struct { const char *field, *dir; } kinds[] = {
		{ "url_image_ingame", "snap" }, { "url_image_title", "titles" }, { "url_image_marquee", "marquees" },
	};
	for (const char *dir : { "snap", "titles", "marquees", "adb" })
		mkdir((root + "/" + dir).c_str(), 0777);
	for (size_t first = 0; curl && first < s_job.sets.size() && !cancelled();) {
		/* As many sets as fit in one query URL. ARCADE_UI_ADB_QUERY points
		 * the queries at a local stand-in for tests. */
		const char *query = getenv("ARCADE_UI_ADB_QUERY");
		std::string url = query && *query ? query : kQuery;
		size_t last = first;
		while (last < s_job.sets.size() &&
		       url.size() + s_job.sets[last].size() + 1 <= kMaxUrl) {
			if (last > first)
				url += ";";
			url += s_job.sets[last++];
		}
		std::string reply;
		std::vector<Record> records;
		if (!fetch_text(curl, url, s_job.ca_file, &reply, &error) || !parse_reply(reply, &records)) {
			if (error.empty())
				error = "unexpected reply from Arcade Database";
			set_progress(NULL, -1, -1, &error);
			break;
		}
		for (const Record &record : records) {
			auto name = record.find("game_name");
			if (name == record.end() || cancelled())
				continue;
			set_progress(&name->second, -1, -1, NULL);
			save_record(root + "/adb/" + name->second + ".json", record);
			for (const auto &kind : kinds) {
				auto image = record.find(kind.field);
				const std::string path = root + "/" + kind.dir + "/" + name->second + ".png";
				if (image == record.end() || image->second.empty() || exists(path) || cancelled())
					continue;
				if (fetch_file(curl, image->second, s_job.ca_file, path, &error))
					set_progress(NULL, -1, ++images, NULL);
				else
					set_progress(NULL, -1, -1, &error);
			}
		}
		done += (int)(last - first);
		set_progress(NULL, done, -1, NULL);
		first = last;
	}
	if (curl)
		curl_easy_cleanup(curl);
	pthread_mutex_lock(&s_lock);
	s_progress.running = false;
	s_progress.finished = true;
	pthread_mutex_unlock(&s_lock);
	return NULL;
}

} // namespace

bool adb_scraper_start(const std::string &data_root, const std::vector<std::string> &sets, const std::string &ca_file)
{
	pthread_mutex_lock(&s_lock);
	if (s_progress.running) {
		pthread_mutex_unlock(&s_lock);
		return false;
	}
	pthread_mutex_unlock(&s_lock);
	adb_scraper_join();
	s_job = Job{ data_root, ca_file, sets };
	s_progress = AdbProgress{ true, false, false, 0, (int)sets.size(), 0, "", "" };
	if (pthread_create(&s_thread, NULL, run, NULL) != 0) {
		s_progress.running = false;
		return false;
	}
	s_thread_started = true;
	return true;
}

AdbProgress adb_scraper_progress(void)
{
	pthread_mutex_lock(&s_lock);
	AdbProgress copy = s_progress;
	pthread_mutex_unlock(&s_lock);
	return copy;
}

void adb_scraper_cancel(void)
{
	pthread_mutex_lock(&s_lock);
	s_progress.cancelled = true;
	pthread_mutex_unlock(&s_lock);
}

void adb_scraper_join(void)
{
	if (s_thread_started) {
		pthread_join(s_thread, NULL);
		s_thread_started = false;
	}
}
