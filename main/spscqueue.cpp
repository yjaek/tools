#include <iostream>
#include <thread>

#include "lib/SPSCQueue.h"

SPSCQueue<int> q{10};

void producer()
{
    for (int i = 0; i < 10; ++i)
    {
        q.push(i);
        std::cout << "Push " << i << std::endl;
    }
}

void consumer()
{
    while (true)
    {
        const int *i = q.front();
        if (i)
        {
            q.pop();
            std::cout << "Pop " << *i << std::endl;
        }
    }
}

int main()
{
    std::thread pthread{producer};
    std::thread cthread{consumer};

    pthread.join();
    cthread.join();
}