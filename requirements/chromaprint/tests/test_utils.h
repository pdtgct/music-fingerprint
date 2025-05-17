#ifndef CHROMAPRINT_TESTS_UTILS_H_
#define CHROMAPRINT_TESTS_UTILS_H_

#include <vector>
#include <fstream>
#include <string>

inline void CheckString(std::string actual, char *expected, int expected_size)
{
	ASSERT_EQ(expected_size, actual.size());
	for (int i = 0; i < expected_size; i++) {
		EXPECT_EQ(expected[i], actual[i]) << "Different at index " << i;
	}
}

inline std::vector<short> LoadAudioFile(const std::string &file_name)
{
	std::ifstream file(file_name.c_str(), std::ifstream::in);
	file.seekg(0, std::ios::end);
	int length = file.tellg();
	file.seekg(0, std::ios::beg);
	std::vector<short> data(length / 2);
	file.read((char *)&data[0], length);
	file.close();
	return data;
}

#endif
