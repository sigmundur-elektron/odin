#pragma once
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

// a directory that removes itself when it leaves scope.
//
// this is the one place the codebase uses a constructor/destructor pair rather
// than plain data plus free functions: a doctest assertion can leave the test
// body early, and cleanup still has to happen.
struct temp_dir
{
	std::filesystem::path path;

	temp_dir()
	{
		static thread_local std::mt19937 engine{std::random_device{}()};
		std::uniform_int_distribution<int> pick(0, 0xffff);

		std::error_code code;
		do
		{
			path = std::filesystem::temp_directory_path() / ("odin_test_" + std::to_string(pick(engine)) + std::to_string(pick(engine)));
		} while (std::filesystem::exists(path));
		std::filesystem::create_directories(path, code);
	}

	~temp_dir()
	{
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}

	temp_dir(const temp_dir &) = delete;
	temp_dir &operator=(const temp_dir &) = delete;
};

// write raw bytes, creating parents. deliberately not file_write_atomic: tests
// should not depend on the code they are testing to build their fixtures.
inline void temp_write(const std::filesystem::path &path, const std::string &contents)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

inline std::string temp_read(const std::filesystem::path &path)
{
	std::ifstream stream(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}
