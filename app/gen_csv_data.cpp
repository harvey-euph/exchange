#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "csv_util.hpp"

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <row_count> [output_csv]\n";
        return 1;
    }

    try {
        const std::string row_count_arg = argv[1];
        size_t parsed_chars = 0;
        const unsigned long long row_count = std::stoull(row_count_arg, &parsed_chars);
        if (parsed_chars != row_count_arg.size() || row_count == 0) {
            throw std::invalid_argument("row_count must be a positive integer");
        }

        if (argc == 3) {
            Exchange::CSVDataGen generator(static_cast<size_t>(row_count), argv[2]);
            generator.run();
        } else {
            Exchange::CSVDataGen generator(static_cast<size_t>(row_count));
            generator.run();
        }
    } catch (const std::exception& ex) {
        std::cerr << "[gen_csv_data] " << ex.what() << '\n';
        return 1;
    }

    return 0;
}
