#pragma once

// A temporary database file whose path is unique per instance and whose file
// is removed on destruction. Replaces the deprecated std::tmpnam used by the
// original examples.

#include <cstdio>
#include <filesystem>
#include <random>
#include <string>

class TmpFile {
public:
	TmpFile()
		: fname((std::filesystem::temp_directory_path()
		         / ("sqlitecpp-" + std::to_string(std::random_device{}()) + ".db")).string()) {}

	~TmpFile() {
		std::remove(fname.c_str());
	}

	TmpFile(const TmpFile &) = delete;
	TmpFile &operator=(const TmpFile &) = delete;

	std::string fname;
};
