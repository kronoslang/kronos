#include <iostream>
#include <fstream>
#include <unordered_map>
#include <functional>
#include <regex>
#include <list>
#include <sstream>
#include <sys/stat.h>

#include "driver/picojson.h"
#include "driver/package.h"
#include "attachment.h"

std::string attach(picojson::value& link, const std::string& path, const std::string& ext, const std::string& mime);

std::string canonicalize_name(std::string name) {
	std::regex sorting_prefix{"^[0-9+]-"};
	return std::regex_replace(name, sorting_prefix, "");
}

picojson::value merge(const picojson::value &lhs, const picojson::value& rhs);

picojson::object merge(picojson::object lhs, const picojson::object& rhs) {
	for (auto& kv : rhs) {
		auto f = lhs.find(kv.first);
		if (f != lhs.end() && f->second.evaluate_as_boolean()) {
			f->second = merge(f->second, kv.second);
		} else if (kv.second.evaluate_as_boolean()) {
			lhs.emplace(kv);
		}
	}
	return lhs;
}

picojson::array merge(picojson::array lhs, const picojson::value& rhs) {
	if (rhs.is<picojson::array>()) {
		auto &ra{ rhs.get<picojson::array>() };
		lhs.insert(lhs.end(), ra.begin(), ra.end());
	} else {
		lhs.emplace_back(rhs);
	}
	return lhs;
}

picojson::value merge(const picojson::value &lhs, const picojson::value& rhs) {
	if (lhs.is<picojson::null>()) return rhs;
	if (rhs.is<picojson::null>()) return lhs;

	if (lhs.is<picojson::object>()) {
		if (rhs.is<picojson::object>()) {
			return merge(lhs.get<picojson::object>(), rhs.get<picojson::object>());
		} else {
			std::cerr << "* Can't merge " << lhs << " with " << rhs << "\n";
			exit(-1);
		}
	}

	if (lhs.is<picojson::array>()) {
		return merge(lhs.get<picojson::array>(), rhs);
	}

	if (rhs.is<picojson::array>()) {
		return merge(rhs.get<picojson::array>(), lhs);
	}

	return picojson::array{ {lhs, rhs} };
}

picojson::value link;

std::function<std::string(std::string)> page_url;

static std::string urlenc(const std::string& raw) {
	std::string result;
	result.reserve(raw.size());
	char percentEncode[4];

	auto proto = raw.find("://");
	if (proto != raw.npos) {
		return raw.substr(0, proto) + "://" + urlenc(raw.substr(proto + 3));
	}

	for (unsigned char c : raw) {
		if (isalnum(c)) result.push_back(c);
		else {
			switch (c) {
			case '\\': result.push_back('/'); break;
			case '-': case '_': case '.': case '~': case '#': case '/':
				result.push_back(c); break;
			default:
				sprintf(percentEncode, "%%%02x", (unsigned char)c);
				result += percentEncode;
				break;
			}
		}
	}
	return result;
}


std::string htmlenc(std::string input) {
	std::string out;
	for (auto c : input) {
		switch (c) {
		default: out.push_back(c); break;
		case '<': out.append("&lt;"); break;
		case '>': out.append("&gt;"); break;
		case '&': out.append("&amp;"); break;
		}
	}
	return out;
}

std::string logical_url;
picojson::value content;

struct tocmaker {
	using key_t = std::tuple<int, std::string>;
	std::map<key_t, tocmaker> children;
	bool is_open = false;

	static key_t make_key(std::string prefix, std::string page) {
		if (link.contains("ordering")) {
			auto ordering = link.get("ordering");
			auto link = prefix + "/" + page;
			if (ordering.contains(link)) {
				return { (int)ordering.get(link).get<double>(), page };
			}
		}
		return { 0, page };
	}

	void place(std::string path, std::string traversed = "") {
		auto slash = path.find('/');
		if (slash != path.npos) {
			auto segment = path.substr(0, slash);
			auto key = make_key(traversed, segment);
			traversed += "/" + segment;
			children[key].place(path.substr(slash + 1), traversed);
		} else if (path != "index") {
			auto key = make_key(traversed, path);
			children[key];
		}
	}

	picojson::array get(const std::string& linkbase, const std::string& linkname, int levels) {
		is_open = false;
		picojson::array item{ {"li"} };
		if (children.size()) {
			auto indexPage = (linkbase + linkname + "/index").substr(1);

			if (!link.get("public").contains(indexPage)) {
				auto& doc = link.get<picojson::object>();
				auto& pages = doc["public"].get<picojson::object>();
				pages[indexPage]; // touch to avoid recursion
				pages[indexPage] = picojson::array{
					"div",
					picojson::array{"h2", "Topics in " + linkname},
					get(linkbase, linkname, 100)[2]
				};
			}

			if (linkname.size()) {
				item.emplace_back(picojson::array{ {
					"link", linkname, page_url(indexPage)
				} });
			}

			// truncate toc here?
			if (levels < 1) return item;

			picojson::array sub{ {"ol"} };
			for (auto &c : children) {
				sub.emplace_back(c.second.get(linkbase + linkname + "/", std::get<std::string>(c.first), levels - 1));
				is_open |= c.second.is_open;
			}

			if (indexPage == logical_url) is_open = true;

			if (linkname.size()) item.emplace_back(sub);
			else return sub;

		} else {
			item.emplace_back(
				picojson::array{ "link", linkname, page_url(linkbase + linkname).substr(1) });
			if (linkbase + linkname == "/" + logical_url) {
				is_open = true;
			}
		}
		item[0] = item[0].to_str() + (is_open ? ".open" : ".closed");
		return item;
	}
} toc;

std::list<std::string> uses = { ":" };

void render_element(std::ostream& os, const picojson::value& data);

void render_link(std::ostream& os, std::string domid, std::string content, std::string url) {
	os << "<a " << domid << " href='" << url << "'>" << htmlenc(content) << "</a>";
}

void render_link(std::ostream& os, std::string domid, picojson::value const & content, std::string url) {
	os << "<a " << domid << " href='" << url << "'>";
	render_element(os, content);
	os << "</a>";
}

std::unordered_map<std::string, std::string> mimeTypes{
	{ ".svg", "image/svg+xml" }
};

std::unordered_map<std::string, std::function<void(std::ostream& os, const picojson::array& element, std::string domid)>> special_tags = {
	{
		"siblink",
		[](std::ostream& os, const picojson::array& element, std::string domid) {
			auto page = element[1].to_str();
			auto title = page;
			if (auto slash = title.find_last_of("/")) {
				title = title.substr(slash + 1);
			}
			os << "<a " << domid << " href='" << urlenc(page_url(page)) << "'>" << title << "</a>";
		}
	},
	{	
		"asset",
		[](std::ostream& os, const picojson::array& element, std::string domid) {
			auto path = link.get("sourcepath").get(logical_url).to_str();
			path = path.substr(0, path.find_last_of('/'));
			auto url = element[2].to_str();
			auto mime = element[1].to_str();
			auto mime_underscore = mime.substr(0, mime.find_first_of(";"));

			for (auto& m : mime_underscore) {
				if (m == '/') m = '_';
			}

			auto uid = attach(link, path + "/" + url, "", mime);
			os << "<div class='asset'><a download='" << url << "' " << domid << " href='/static/asset/" << uid << "'><img src='/static/mime_" << mime_underscore
				<< ".png' alt='" << mime << "'>" << htmlenc(url) << "</a></div>";
		}
	},
	{
		"html",
		[](std::ostream& os, const picojson::array& element, std::string domid) {
			for (int i = 1; i < element.size(); ++i) os << element[i].to_str();
		}
	},
	{
		"ref",
		[](std::ostream& os, const picojson::array& element, std::string idattr) {
			os << "<div class='reference-link' id='Reference:" << element[1].to_str() << "'>";
			os << "<span class='reference-title'>" << htmlenc(element[1].to_str()) << "</span> ";
			os << "<a href='" << urlenc(element[2].to_str()) << "'>" << element[2].to_str() << "</a></div>";
	}
	},
	{
		"link",
		[](std::ostream& os, const picojson::array& element, std::string domid) {
			switch (element.size()) {
			case 3:
				render_link(os, domid, element[1], element[2].to_str());
				break;
			case 2:
				render_link(os, domid, element[1].to_str(), "#Reference:" + urlenc(element[1].to_str()));
				break;
			default:
				std::cerr << "malformed link" << picojson::value{ element } << std::endl;
				exit(-1);
			}
		}
	},
	{
		"img",
		[](std::ostream& os, const picojson::array& element, std::string domid) {
			auto path = link.get("sourcepath").get(logical_url).to_str();
			path = path.substr(0, path.find_last_of('/'));
			auto url = element[2].to_str();

			if (url.find("//") == url.npos) {
				auto ext = url.substr(url.find_last_of('.'));

				std::string mime;
				
				if (mimeTypes.count(ext)) mime = mimeTypes[ext];
				else mime = "image/" + ext.substr(1);

				auto uid = attach(link, path + "/" + url, ext, mime);

				os << "<img class='asset' alt='" << element[1].to_str() << "' src='/static/asset/" << uid << "'>";
			} else {
				os << "<img class='asset external' alt='" << element[1].to_str() << "' src='" << url << "'>";
			}
		}
	},
	{
		"meta",
		[](std::ostream& os, const picojson::array& element, std::string idattr) {
			os << "<meta ";
			for (int i = 1; i + 1 < element.size(); i += 2) {
				os << element[i].to_str() << "='" << element[i + 1].to_str() << "' ";
			}
			os << "/>";
		}
	},
	{
		"current-path",
		[](std::ostream& os, const picojson::array& element, std::string domid) {
			os << "<ul class='path'>";
			for (auto seg = 0ull;;) {
				auto next = logical_url.find('/', seg);
				if (next == logical_url.npos) {
					os << "<li class='path page'>" << logical_url.substr(seg) << " </li>";
					break;
				} else {
					os << "<li class='path'>" << logical_url.substr(seg, next - seg) << "</li>";
					seg = next + 1;
				}
			}
			os << "</ul>";
		}
	},
	{
		"page-name",
		[](std::ostream& os, const picojson::array& element, std::string domid) {
			os << logical_url;
		}},
		{
			"last-modified",
			[](std::ostream& os, const picojson::array& element, std::string domid) {
			if (link.contains("modified") && link.get("modified").contains(logical_url)) {
				auto isodate = link.get("modified").get(logical_url).to_str();
				int y, M, d, h, m;
				float s;
				sscanf(isodate.c_str(), "%d-%d-%dT%d:%d:%fZ", &y, &M, &d, &h, &m, &s);
				tm time;
				time.tm_year = y - 1900;
				time.tm_mon = M - 1;
				time.tm_mday = d;
				time.tm_hour = h;
				time.tm_min = m;
				time.tm_sec = (int)s;
				char buf[256];
				strftime(buf, 256, element[1].to_str().c_str(), &time);
				os << "<span " << domid << " class='date-time'>" << buf << "</span>";
			}
	}
		},
	{
		"toc",
		[](std::ostream& os, const picojson::array& element, std::string domid) {
			picojson::array tocData;
			if (element.size() > 1 && element[1].is<double>()) {
				int depth_limit = (int)element[1].get<double>();
				tocData = toc.get("", "", depth_limit);
			} else {
				tocData = toc.get("", "", 1000);
			}

			render_element(os, tocData);
	}
	},
	{
		"content",
		[](std::ostream& os, const picojson::array& element, std::string domid) {
			render_element(os, content);
	}
	},
	{
		"stylesheet",
		[](std::ostream& os, const picojson::array& element, std::string idattr) {
			os << "<link rel='stylesheet' href='" << element[1].to_str() << "'>";
	}
	},
	{
		"inline-stylesheet",
		[](std::ostream& os, const picojson::array& element, std::string idattr) {
			if (link.contains("resources")) {
				auto res = link.get("resources");
				auto fn = element[1].to_str();
				if (res.contains(fn)) {
					os << "<style>\n" << res.get(fn).to_str() << "</style>";
					return;
				}
			}
			os << "<!-- missing " << element[1].to_str() << "-->";
		}
	},
	{
		"Use",
		[](std::ostream& os, const picojson::array& element, std::string idattr) {
			os << "<span><span class='kronos-syntax keyword'>Use </span>";
			for (int i = 1; i < element.size(); ++i) render_element(os, element[i]);
			os << "</span>";
			auto name = element[1].to_str();
			for (auto &u : uses) {
				uses.push_front(u + name + ":");
			}
	}
	}
};

void render_element(std::ostream& os, const picojson::value& data) {
	if (data.is<picojson::array>()) {
		auto element = data.get<picojson::array>();
		if (element[0].is<picojson::null>()) {
			for (int i = 1;i < element.size();++i) {
				render_element(os, element[i]);
			}
			return;
		}
		auto tag = element[0].to_str();

		std::string idattr;

		if (tag.find('#') != tag.npos) {
			auto p = tag.find('#');
			idattr = " id='" + tag.substr(p + 1) + "'";
			tag = tag.substr(0, p);
		}

		std::string cls;
		std::string baretag = tag;
		size_t offset = tag.find('.');
		while (offset != tag.npos) {
			if (cls.size()) cls += " ";
			auto next = tag.find('.', offset + 1);
			cls += tag.substr(offset + 1, next - offset - 1);
			offset = next;
		}
		
		tag = tag.substr(0, tag.find('.'));
	
		auto f = special_tags.find(tag);
		if (f != special_tags.end()) {
			if (cls.size()) cls = " class='" + cls + "'";
			f->second(os, element, idattr + cls);
			return;
		}

		if (tag == "audio") {
			os << "<audio preload='none' controls><source src='/static/asset/";
			for (int i = 1;i < element.size();++i) {
				render_element(os, element[i]);
			}
			os << "' type='audio/mpeg'></audio>";
		} else {
			tag = tag.substr(0, tag.find("."));

			os << "<" << tag << idattr;
			if (cls.size()) {
				os << " class='" << cls << "'";
			}

			os << ">";
			for (int i = 1; i < element.size(); ++i) {
				render_element(os, element[i]);
			}
			os << "</" << tag << ">";
		}
	} else {
		auto word = data.to_str();

		if (word.size() && link.contains("symbols")) {
			auto& syms = link.get("symbols").get<picojson::object>();
			if (word.front() == ':') {
				auto f = syms.find(word.substr(1));
				if (f != syms.end()) {
					os << "<a class='symbol-reference' href='/" << urlenc(page_url(f->second.to_str())) << "#" << f->first << "'>" << word << "</a>";
					return;
				}
			}

			for (auto &u : uses) {
				auto f = syms.find(u.substr(1) + word);
				if (f != syms.end()) {
					os << "<a class='symbol-reference' href='/" << urlenc(page_url(f->second.to_str())) << "#" << f->first << "'>" << word << "</a>";
					return;
				}
			}
		}

		os << htmlenc(word);
	}	
}

void copy_attachments() {
	static const std::uint8_t pr2six[256] =
	{
		/* ASCII table */
		64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 62, 64, 64, 64, 63,
		52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 64, 64, 64, 64, 64, 64,
		64,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
		15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 64, 64, 64, 64, 64,
		64, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
		41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 64, 64, 64, 64, 64,
		64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
		64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64
	};

	if (link.contains("~attachments")) {
 		for (auto &a : link.get("~attachments").get<picojson::object>()) {
			struct stat st;
			if (stat(("assets/" + a.first).c_str(), &st)) {
				Packages::MakeMultiLevelPath("assets/");
				std::ofstream file{ "assets/" + a.first, std::ios_base::binary };
				std::clog << " - " << a.second.get("name") << "(" << a.first << ")\n";
				if (file.is_open() == false) throw std::runtime_error("Could not write 'assets/" + a.first + "'");
				auto decode = b64decode(a.second.get("data").to_str());
				file.write((const char*)decode.data(), decode.size());
			} else {
				std::clog << " - " << a.first << " exists\n";
			}
		} 
	}
}


void render_pages() {
	for (auto&p : link.get("public").get<picojson::object>()) {
		toc.place(p.first);
	}

	for (auto &p : link.get("public").get<picojson::object>()) {
		logical_url = p.first;
		auto url = page_url(p.first);
		std::clog << "- " << url << "\n";
		std::ofstream write{ url };
		if (!write.is_open()) {
			std::cerr << "Can't write to " << url << "\n";
			exit(-1);
		}

		picojson::value tmpl;
		if (link.contains("template")) {
			auto ts = link.get("template");
			if (ts.is<picojson::object>()) {
				for (auto &kv : ts.get<picojson::object>()) {
					std::regex re{ kv.first };
					if (std::regex_match(p.first, re)) {
						tmpl = kv.second;
					}
				}
			} else {
				tmpl = ts;
			}
		}

		if (tmpl.is<picojson::null>()) {
			tmpl = p.second;
		} else {
			content = p.second;
		}

		write << "<!DOCTYPE html>";
		render_element(write, tmpl);
	}
}

void render_abstract(std::ostream& stream, picojson::value const & article) {
	if (article.is<picojson::array>()) {
		auto &page{ article.get<picojson::array>() };
		for (int i = 1; i < page.size(); ++i) {
			auto &e{ page[i] };
			render_element(stream, e);
			if (e.is<picojson::array>() && e.get(0) == "p") {
				return;
			}
		}
	}
}


void render_to_db(const char* url, const char *auth) {
	using namespace std::string_literals;
	std::string dburl{ url };

	std::unordered_set<std::string> requestHeaders{
		"Content-Type: application/json"
	};

	if (strlen(auth)) {
		requestHeaders.emplace("Authorization: "s + auth);
	}

	std::unordered_map<std::string, std::string> current;

	std::clog << "Syncing to " << dburl << "\n";

	for (auto &p : link.get("public").get<picojson::object>()) {
		logical_url = p.first;
		auto url = page_url(p.first);
		std::clog << "- " << url << "\n";
		std::stringstream write{ url };
		render_element(write, p.second);
		auto content = write.str();

		write.str("");
		render_abstract(write, p.second);
		auto abstract = write.str();

		auto modified = link.get("modified").get(logical_url);
		picojson::value created;
		if (link.contains("created") && link.get("created").contains(logical_url)) {
			created = link.get("created").get(logical_url);
		}

		picojson::object document{
			{"content", content },
			{"abstract", abstract },
			{"url", logical_url },
			{"modified", modified },
			{"created", created }
		};

		if (link.contains("markdown") && link.get("markdown").contains(p.first)) {
			document["markdown"] = link.get("markdown").get(p.first).to_str();
		}

		auto doc_ser = picojson::value{ document }.serialize();
		auto content_hash = Packages::fnv1a(doc_ser.data(), doc_ser.size());

		current[logical_url] = content_hash;

		if (WebRequest("PUT", dburl, "ananke/" + content_hash.to_str(), doc_ser.data(), doc_ser.size(),
					   requestHeaders).Ok()) {
			std::clog << "Uploaded to database\n";
		}
	}

	copy_attachments();

	for (auto &a : link.get("~attachments").get<picojson::object>()) {
		std::ifstream fs{ "assets/" + a.first, std::ios_base::binary };
		if (fs.is_open()) {
			fs.seekg(0, std::ios_base::end);
			std::vector<char> fileData((size_t)fs.tellg());
			fs.seekg(0, std::ios_base::beg);
			fs.read(fileData.data(), fileData.size());

			auto assetUrl = "ananke/" + a.first;

			auto webr = WebRequest("GET", dburl, assetUrl);
			if (!webr.Ok() || !((picojson::value)webr).contains("_id")) {
				std::clog << "\n- Uploading '" << a.second.get("name") << "'\n";

				picojson::object blob{
					{ "type", "asset" },
					{ "source", a.second.get("name") }
				};
				auto blobStr = picojson::value{ blob }.serialize();

				auto resp = WebRequest("PUT", dburl, assetUrl, blobStr.data(), blobStr.size(),
									   requestHeaders).JSON();

				if (!resp.contains("ok")) {
					if (resp.get("error") == "conflict") {
						std::clog << "- Already exists\n";
						continue;
					}
					throw std::runtime_error("Database error while deduplicating " + a.first + ": " + resp.serialize());
				}

				std::unordered_set<std::string> uploadHeaders{
					"Content-Type: " + a.second.get("mime").to_str()
				};

				if (strlen(auth)) {
					uploadHeaders.emplace("Authorization: "s + auth);
				}

				picojson::value putr = WebRequest("PUT", dburl, assetUrl + "/data?rev=" + resp.get("rev").to_str(),
												  fileData.data(), fileData.size(),
												  uploadHeaders);

				if (!putr.contains("ok")) {
					std::clog << WebRequest("DELETE", dburl, assetUrl + "?rev=" + putr.get("rev").to_str(),
											nullptr, 0, requestHeaders).data.data();
					throw std::runtime_error("* Failed to post attachment '" + a.first + "': " + resp.serialize());
				}
			}
		}
	}

	auto digest = WebRequest("GET", dburl, "ananke/_design/d/_view/digest",
							 nullptr, 0, requestHeaders).JSON();
	if (digest.contains("error")) {
		throw std::runtime_error(digest.get("error").to_str());
	}
	auto digest_rows = digest.get("rows").get<picojson::array>();
	for (auto &d : digest_rows) {
		if (d.is<picojson::object>()) {
			auto& row{ d.get<picojson::object>() };
			auto url = row["key"].to_str();
			auto fnv = row["id"].to_str();
			if (fnv != current[url]) {
				std::clog << "Deleting stale " << url << " (" << fnv << " -> " << current[url] << ")\n";
				picojson::object deleteDoc{
					{"_rev", row["value"]},
					{"_deleted", true}
				};
				auto delete_ser = picojson::value{ deleteDoc }.serialize();
				WebRequest("PUT", dburl, "ananke/" + row["id"].to_str(), 
						   delete_ser.data(), delete_ser.size(), requestHeaders);

			}
		}
	}

}

int link_page(const char **files, int num_files, const char* db_url, const char *db_auth) {
	try {
		std::clog << "Linking... " << (db_auth ? "(database)" : "(local files)") << "\n";

		link = picojson::object{};
		for (int i = 0; i < num_files; ++i) {
			std::clog << " - " << files[i] << "\n";

			std::ifstream json(files[i]);
			if (!json.is_open()) {
				std::cerr << files[i] << " not found.\n";
				//exit(-1);
				continue;
			}
			picojson::value unit;

			auto err = picojson::parse(unit, json);
			if (!err.empty()) {
				std::cerr << "\n" << err;
				std::cerr << unit << "\n";
				exit(-1);
			}

			link = merge(link, unit);
		}

		std::ofstream tmp{ "amalgam.json" };
		tmp << link;

		if (link.contains("tests")) {
			auto writeFile = [](const std::string & name, const std::string& content) {
				std::ofstream write{ name };
				if (!write.is_open()) throw std::runtime_error("Could not write '" + name + "'");
				write << content;
			};

			Packages::MakeMultiLevelPath("tests/");

			for (auto&& t : link.get("tests").get<picojson::object>()) {
				auto&& name = t.first;
				auto&& data = t.second.get<picojson::object>();
				std::string source;
				auto& src = data.at("source");
				
				if (src.is<picojson::array>()) {
					for (auto& s : src.get<picojson::array>()) {
						source += s.to_str();
						source.push_back('\n');
					}
				} else {
					source = src.get<std::string>();
				}

				writeFile("tests/" + name + ".k", source);
				writeFile("tests/" + name + ".json", picojson::value{
					picojson::object {
						{ "audio", data.at("audio") },
						{ "eval", data.at("eval") } }
				}.serialize());
			}
		}

		std::clog << "\n";

		copy_attachments();
		if (db_url && db_auth) {
			if (!strcmp(db_auth, "anonymous")) db_auth = "";
			page_url = [](std::string path) {
				for (auto& c : path) {
					if (c == '/') { } else if (isalnum(c) || c == '.' || c == '-' || c == '_') { c = tolower(c); } else { c = '-'; }
				}
				return path;
			};
			try {
				render_to_db(db_url, db_auth);
			} catch (std::exception & e) {
				std::cerr << e.what() << "\n";
				return -1;
			}
		} else {
			page_url = [](std::string path) {
				for (auto& c : path) {
					if (c == '/') c = '.';
				}
				return path + ".html";
			};
			render_pages();
			copy_attachments();
		}
		return 0;
	} catch (std::exception & e) {
		std::clog << e.what() << "\n";
		return -1;
	}
}
