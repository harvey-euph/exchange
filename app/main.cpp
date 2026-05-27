#include <iostream>
#include <queue>

#include "OrderBook.hpp"
#include "OrderRequestReader.hpp"

using namespace Exchange;

int main()
{
    // one-symbol for now
    OrderBook ob(10000, 2000, 8192);
    
    OrderRequestReader reader;
    reader.loadFromCSV("benchmarks/bmark1.csv");
    std::vector<const OrderRequest*> vec = reader.getRequests();
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