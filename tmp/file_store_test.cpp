#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include "file_store.h"

void testSimpleOperations()
{
    std::cout << "------Test Simple Operations------" << std::endl;

    FileStore store("data/test_storage_SimpleOperations.dat", 4, 100);

    std::cout << "Putting key 1: 'hello'" << std::endl;
    store.put(1, "hello");

    std::cout << "Putting key 2: 'world'" << std::endl;
    store.put(2, "world");

    std::cout << "Getting key 1: " << store.get(1) << std::endl;
    std::cout << "Getting key 2: " << store.get(2) << std::endl;

    std::cout << "Deleting key 1" << std::endl;
    store.del(1);

    std::cout << "Putting key 1: 'new_hello'" << std::endl;
    store.put(1, "new_hello");

    std::cout << "Getting key 1: " << store.get(1) << std::endl; // "new_hello"
    std::cout << "Getting key 2: " << store.get(2) << std::endl; // "world"

    std::cout << "Deleting key 2" << std::endl;
    store.del(2);

    std::cout << "Putting key 2: 'new_world'" << std::endl;
    store.put(2, "new_world");

    std::cout << "Getting key 1: " << store.get(1) << std::endl; // "new_hello"
    std::cout << "Getting key 2: " << store.get(2) << std::endl; // "new_world"

    // std::this_thread::sleep_for(std::chrono::seconds(2)); // 让垃圾回收线程有时间触发
}

void testComplexOperations()
{
    std::cout << "------Test Complex Operations------" << std::endl;

    FileStore store("data/test_storage_ComplexOperations.dat", 4, 200);

    std::cout << "Inserting key 1: 'hello'" << std::endl;
    store.put(1, "hello");
    std::cout << "Inserting key 2: 'world'" << std::endl;
    store.put(2, "world");

    std::cout << "Getting key 1: " << store.get(1) << std::endl; //  "hello"
    std::cout << "Getting key 2: " << store.get(2) << std::endl; //  "world"

    std::cout << "Deleting key 1" << std::endl;
    store.del(1);

    std::cout << "Inserting key 1: 'new_hello'" << std::endl;
    store.put(1, "new_hello");

    std::cout << "Getting key 1: " << store.get(1) << std::endl; //  "new_hello"
    std::cout << "Getting key 2: " << store.get(2) << std::endl; //  "world"

    std::cout << "Inserting key 3: 'foo'" << std::endl;
    store.put(3, "foo");
    std::cout << "Inserting key 4: 'bar'" << std::endl;
    store.put(4, "bar");

    std::cout << "Getting key 3: " << store.get(3) << std::endl; //  "foo"
    std::cout << "Getting key 4: " << store.get(4) << std::endl; //  "bar"

    std::cout << "Deleting key 2" << std::endl;
    store.del(2);

    std::cout << "Inserting key 2: 'new_world'" << std::endl;
    store.put(2, "new_world");

    std::cout << "Getting key 2: " << store.get(2) << std::endl; //  "new_world"

    std::cout << "Deleting key 3" << std::endl;
    store.del(3);

    std::cout << "Inserting key 3: 'updated_foo'" << std::endl;
    store.put(3, "updated_foo");

    std::cout << "Getting key 3: " << store.get(3) << std::endl; //  "updated_foo"

    std::cout << "Inserting keys 5 to 20" << std::endl;
    for (int i = 5; i <= 20; ++i)
    {
        store.put(i, "data_" + std::to_string(i));
    }

    std::cout << "Getting key 4: " << store.get(4) << std::endl;   //  "bar"
    std::cout << "Getting key 10: " << store.get(10) << std::endl; //  "data_10"

    std::cout << "Deleting keys 4 and 10" << std::endl;
    store.del(4);
    store.del(10);

    std::cout << "Inserting key 4: 'reinserted_bar'" << std::endl;
    store.put(4, "reinserted_bar");
    std::cout << "Inserting key 10: 'reinserted_data_10'" << std::endl;
    store.put(10, "reinserted_data_10");

    std::cout << "Getting key 4: " << store.get(4) << std::endl;   //  "reinserted_bar"
    std::cout << "Getting key 10: " << store.get(10) << std::endl; //  "reinserted_data_10"

    std::cout << "Inserting keys 21 to 30" << std::endl;
    for (int i = 21; i <= 30; ++i)
    {
        store.put(i, "data_" + std::to_string(i));
    }

    std::cout << "Getting key 1: " << store.get(1) << std::endl;   //  "new_hello"
    std::cout << "Getting key 2: " << store.get(2) << std::endl;   //  "new_world"
    std::cout << "Getting key 3: " << store.get(3) << std::endl;   //  "updated_foo"
    std::cout << "Getting key 4: " << store.get(4) << std::endl;   //  "reinserted_bar"
    std::cout << "Getting key 10: " << store.get(10) << std::endl; //  "reinserted_data_10"

    // std::this_thread::sleep_for(std::chrono::seconds(2)); // 让垃圾回收线程有时间触发
}

int main()
{
    // testSimpleOperations();
    testComplexOperations();

    // 等待垃圾回收线程运行
    // std::this_thread::sleep_for(std::chrono::seconds(2)); // 让垃圾回收线程有时间触发

    return 0;
}
