#include <iostream>
#include <queue>
#include "OrderBook.hpp"
#include "OrderRequestReader.hpp"

using namespace Exchange;

int main()
{
    // one-symbol for now
    OrderBook ob(1, 10000, 20000);
    OrderRequestReader reader;

    // get from csv
    reader.loadFromCSV("data/testReqs.csv");
    auto vec = reader.getRequests();
    std::queue<const OrderRequest*> q;
    for (size_t i = 0; i < vec.size(); i++) q.push(vec[i]);

    while (!q.empty())
    {
        const OrderRequest* req = q.front();
        q.pop();
        
        ob.processRequest(req);
        ob.showL2();
    }

    return 0;
}