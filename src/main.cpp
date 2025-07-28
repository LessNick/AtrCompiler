#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <sstream>
#include <memory>
#include <cassert>
#include "../libatr/libatr.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <vector>
#include <sys/types.h>
#include <sys/stat.h>

// http://jsoncpp.sourceforge.net/
#include "json/json.h"

#include "md5.h"

using namespace std;

void make_dir(const std::string & name)
{
#if defined(_WIN32)
	_mkdir(name.c_str());
#else 
	mkdir(name.c_str(), 0777);
#endif
}

void change_dir(const std::string & name)
{
#if defined(_WIN32)
	_chdir(name.c_str());
#else 
	chdir(name.c_str());
#endif
}

bool iequals(const char * s1, const char * s2)
{
#if defined(_WIN32)
	return _stricmp(s1, s2) == 0;
#else
	return strcasecmp(s1, s2) == 0;
#endif
}

/*

Index file
==========

DISK  density
BOOT  filename
FS    filesystem [dos2]

filename [atarifilename]
--- atarifilename
BIN filename atarifilename
DOS filename atarifilename -- will install the file as DOS.SYS

   */

enum class file_format {
	empty,
	bin,
	dos,
	dir
};

bool is_reserved_filename(const string filename)
{
	const char * names[] = { "con", "aux", "nul", "prn",
					"com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9",
					"lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
					nullptr};

	for (const char ** p = names; *p != nullptr; p++) {
		if (iequals(filename.c_str(), *p)) return true;
	}
	return false;
}

disk * pack(const string dir_filename)
{

	std::vector<filesystem::dir *> dir_stack;
	ifstream index(dir_filename);

	size_t sector_size = 128;
	disk::sector_num sector_count = 1040;
	string dos_type;

	disk * d = nullptr;
	filesystem * fs = nullptr;
	const filesystem::property * prop;
	filesystem::dir * dir = nullptr;;

	while (!index.eof()) {
		string line;
		getline(index, line);
		if (line.size() == 0) continue;
		if (line[0] == ';') continue;

		istringstream s(line);

		//parse file name or command

		string filename;

		int nesting = 0;

		s >> filename;

		while (filename == "|") {
			nesting++;
			s >> filename;
		}

		if (nesting > dir_stack.size()) {
			throw "invalid dir nesting";
		}

		while (nesting < dir_stack.size()) {
			delete dir;
			dir = dir_stack.back();
			dir_stack.pop_back();
			change_dir("..");
		}

		file_format fformat = file_format::bin;

		if (filename == "DISK") {
			s >> sector_count >> sector_size;
			continue;

		} else if (filename == "FORMAT") {
			s >> dos_type;
			if (!d) d = new disk(sector_size, sector_count);
			fs = install_filesystem(d, dos_type);
			dir = fs->root_dir();
			continue;

		} else if (filename == "BOOT") {
			if (!d) d = new disk(sector_size, sector_count);
			s >> filename;
			d->install_boot(filename);
			continue;

		} else if (fs && (prop = fs->find_property(filename))) {
			string value;
			getline(s, value);
			fs->set_property(prop, value);
			continue;
		} else if (filename == "/") {
			fformat = file_format::dir;
			s >> filename;
			change_dir(filename.c_str());
		}  else if (filename == "---") {
			fformat = file_format::empty;
		} else {
			if (filename == "BIN") {
				fformat = file_format::bin;
				s >> filename;
			} else if (filename == "DOS") {
				fformat = file_format::dos;
				s >> filename;
			} else {
				fformat = file_format::bin;
			}
		}

		// parse atari name

		char name[12];
		if (!filesystem::format_atari_name(s, name, 8, 3)) {
			istringstream fs(filename);
			filesystem::format_atari_name(fs, name, 8, 3);
		}

		for (auto i = 0; i < nesting; i++) cout << " | ";

		if (fformat == file_format::dir) {
			dir_stack.push_back(dir);
			dir = dir->create_dir(name);
			cout << filename << "/\n";
		} else {
			auto file = dir->create_file(name);
			if (fformat != file_format::empty) {
				if (filename.size() == 0) {
					throw "no filename";
				}
				cout << filename << "\n";
				file->import(filename);
				if (fformat == file_format::dos) {
					auto pos = file->first_sector();
					fs->set_dos_first_sector(file->first_sector());
				}
			}
			delete file;
		}
	}

	while (dir_stack.size()) {
		delete dir;
		dir = dir_stack.back();
		dir_stack.pop_back();
		change_dir("..");
	}
	delete dir;

	delete fs;
	return d;
}

void unpack_dir(filesystem::dir * dir, ofstream & atrdir, int nesting, disk::sector_num dos_first_sector)
{
	int name_idx = 1;
	for (; !dir->at_end(); dir->next()) {

		if (dir->is_deleted()) continue;

		string name = dir->name();
		
		for (int i = 0; i < nesting; i++) {
			cout << " | ";
			if (atrdir.is_open()) {
				atrdir << " | ";
			}
		}

		cout << std::setfill(' ') << std::setw(12) << std::left << name  << " ";

		if (dir->is_dir()) {
			cout << " /\n";
			auto subdir = dir->open_dir();
			if (atrdir.is_open()) {
				atrdir << "/ " << name << "\n";
				make_dir(name);
				change_dir(name.c_str());
			}
			unpack_dir(subdir, atrdir, nesting + 1, dos_first_sector);
			delete subdir;
			if (atrdir.is_open()) {
				change_dir("..");
			}
		} else {
			cout << std::right << std::setw(7) << dir->size() << std::setw(4) << dir->sec_size() << "\n";

			if (atrdir.is_open()) {
				if (dir->size() == 0) {
					atrdir << "--- ";
					atrdir << name;
				} else {

					auto file = dir->open_file();

					if (file->first_sector() == dos_first_sector) {
						atrdir << "DOS ";
					}

					if (name.find('\\') != string::npos) {
						ostringstream s;
						s << "F" << name_idx << ".bin";
						auto p = name.find('.');
						if (p != string::npos) {
							s << name.substr(p);
						}
						atrdir << s.str() << " " << name;
						name = s.str();
					} else {
						atrdir << name;
					}

					file->save(name);
					delete file;
				}
				atrdir << "\n";
			}
		}
	}
}

void unpack(filesystem * fs, const string & dir_file)
{
	ofstream atrdir;
	if (!dir_file.empty()) {
		atrdir.open(dir_file);
	}

	cout << "DISK " << fs->sector_count() << " " << fs->sector_size() << "\n";
	cout << "FORMAT " << fs->name() << "\n";
	cout << "\n";

	if (atrdir.is_open()) {
		atrdir << "DISK " << fs->sector_count() << " " << fs->sector_size() << "\n";
		atrdir << "FORMAT " << fs->name() << "\n";

		string boot_filename = "boot.bin";
		atrdir << "BOOT " << boot_filename << "\n";
		fs->get_disk()->save_boot(boot_filename);
	}

	for (auto prop = fs->properties(); prop->name; prop++) {
		string value = fs->get_property(prop);
		atrdir << prop->name << " " << value << "\n";
	}

	auto dos_first_sector = fs->get_dos_first_sector();

	auto dir = fs->root_dir();

	unpack_dir(dir, atrdir, 0, dos_first_sector);

	delete dir;

	cout << "\n" << "free sectors: " << fs->free_sector_count() << "\n";

}

Json::Value unpackDirJson(filesystem::dir* dir, int nesting, disk::sector_num dos_first_sector) {
	Json::Value dirList;

	int idx = 0;
	for (; !dir->at_end(); dir->next()) {
		if (dir->is_deleted()) continue;

		Json::Value fileInfo;

		fileInfo["name"] = dir->name();

	if (dir->is_dir()) {
		auto subdir = dir->open_dir();
		fileInfo["type"] = "dir";
		fileInfo["sub_dir"] = unpackDirJson(subdir, nesting + 1, dos_first_sector);
		delete subdir;
	
	} else {
		fileInfo["type"] = "file";

		auto file = dir->open_file();
		unsigned char md5_value[16];
		MD5_CTX md5;

		unsigned char data[1];

		MD5Init(&md5);
		for (int i = 0; i < dir->size(); i++) {
			data[0] = file->read();
			MD5Update(&md5, data, 1);
		}

		MD5Final(&md5, md5_value);

		fileInfo["size"] = dir->size();
		fileInfo["sec_size"] = dir->sec_size();

		char md5_str[16*2 + 1] = { 0 };

		char tmp[100] = { 0 };
		memset(md5_str, 0, 16*2 + 1);

		for (int i = 0; i < 16; i++) {
			sprintf_s(tmp, "%02x", md5_value[i]);
			strcat_s(md5_str, tmp);
		}

		fileInfo["md5"] = md5_str;

	}
		dirList[idx] = fileInfo;
		idx++;
	}
	return dirList;
}

void unpackJson(filesystem * fs, string fileName) {

	Json::Value root;
	Json::Value info;
	Json::Value dirList;

	info["sector_count"] = fs->sector_count();
	info["sector_size"] = fs->sector_size();
	info["format"] = fs->name();

	auto dos_first_sector = fs->get_dos_first_sector();

	auto dir = fs->root_dir();

	dirList = unpackDirJson(dir, 0, dos_first_sector);

	delete dir;

	info["free_sectors"] = fs->free_sector_count();

	root["info"] = info;
	root["dir"] = dirList;

	Json::StreamWriterBuilder builder;
	const std::string json_file = Json::writeString(builder, root);
	if (fileName != "") {
		std::ofstream out(fileName);
		out << json_file;
		out.close();
	} else {
		std::cout << json_file << std::endl;
	}

}

const string help =
"AtrCompiler v0.6\n"
"----------------\n"
"https://github.com/rudla/AtrCompiler\n"
"https://github.com/LessNick/AtrCompiler\n"
"\n"
"Usage:\n"
"AtrCompiler json   atr_file [out_filename]\n"
"AtrCompiler list   atr_file\n"
"AtrCompiler pack   atr_file [dir_file]\n"
"AtrCompiler unpack atr_file [dir_file]\n"
"\n";

void disk_cache_test()
{
	disk::sector * s1;
	auto d = new disk(128, 100);

	for (int i = 1; i < 10; i++) {
		s1 = d->init_sector(i);
		s1->poke(0, i);
	}
	
	s1 = d->get_sector(3);
	assert(s1->peek(0) == 3);

	s1 = d->get_sector(5);
	assert(s1->peek(0) == 5);

	s1 = d->get_sector(5);
	assert(s1->peek(0) == 5);

	s1 = d->get_sector(5);
	assert(s1->peek(0) == 5);

	s1 = d->get_sector(3);
	assert(s1->peek(0) == 3);

}


int main(int argc, char *argv[])
{
	/*

	pack   .atr [dirfile]
	unpack .atr [dirfile]
	list   .atr

	*/

	//disk_cache_test();

	string command;
	int x = 1;
	try {
		if (argc == 1) {
			cout << help;
		} else {
			if (strcmp(argv[x], "json") == 0) {
				x++;
				auto d = disk::load(argv[x++]);
				auto fs = detect_filesystem(d);
				string fileName = "";
				if (x < argc) {
					fileName = argv[x++];
				}
				unpackJson(fs, fileName);

			} else if (strcmp(argv[x], "list") == 0) {
				x++;
				auto d = disk::load(argv[x++]);
				auto fs = detect_filesystem(d);
				unpack(fs, "");
			
			} else if (strcmp(argv[x], "pack") == 0) {
				x++;
				string atr = argv[x++];
				string dir = "dir.txt";
				if (x < argc) {
					dir = argv[x++];
				}
				auto d2 = pack(dir);
				d2->save(atr);
				delete d2;
			
			} else if (strcmp(argv[x], "unpack") == 0) {
				x++;
				string atr = argv[x++];
				string dir = "dir.txt";
				auto d = disk::load(atr);
				auto fs = detect_filesystem(d);

				if (x < argc) {
					char* unpackPath = argv[x++];

					struct stat info;

					if (stat(unpackPath, &info) != 0) {
						auto status = _mkdir(unpackPath);
						if (status == -1) {
							cerr << "Error: Can;t create \"" << unpackPath << "\" directory\n";
							return 2;
						}
					}

					auto status = _chdir(unpackPath);
					if (status == -1) {
						cerr << "Error: Can;t change \"" << unpackPath << "\" directory\n";
						return 2;
					}

				}

				unpack(fs, dir);
			}
		}
	} catch (const char * msg) {
		cerr << "Error: " << msg << "\n";
	}
	return 0;
}
