#include <iostream>

#include "OrderBook.hpp"
#include "csv_util.hpp"

using namespace Exchange;

int main(int argc, char** argv)
{
    const char* csv_path = argc > 1 ? argv[1] : "data/basic.csv";

    // one-symbol for now
    OrderBook ob(10000, 2000, 8192);
    
    CSVDataReader reader;
    if (!reader.loadFromCSV(csv_path)) {
        return 1;
    }

    for (const OrderRequest* req : reader.getRequests()) {
        ob.processRequest(req);
        ob.showL2();
    }

    return 0;
}
