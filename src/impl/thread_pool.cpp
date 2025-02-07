#include "thread_pool.hpp"

// initialize static members
thread_local ThreadPool::ThreadLocalRandomData ThreadPool::random_data;

ThreadPool::ThreadPool(size_t numThreads)
{
    thread_data.reserve(numThreads);
    workers.reserve(numThreads);

    const int num_cores = std::thread::hardware_concurrency();

    // initialize thread local data for each worker
    for (size_t i = 0; i < numThreads; i++)
    {
        thread_data.push_back(std::make_unique<ThreadData>(i));
    }

    // start worker threads
    for (size_t i = 0; i < numThreads; i++)
    {
        workers.emplace_back([this, i]
                             { worker_thread(i); });
    }

    Logger::getInstance()->success(
        "thread pool initialized with " + std::to_string(numThreads) +
        " threads" +
        (num_cores > 0 ? " across " + std::to_string(num_cores) + " CPU cores"
                       : " (CPU core count unknown)") +
        " with work stealing and memory pool enabled");
}

bool ThreadPool::steal_task(std::function<void()> &task, size_t self_id)
{
    auto &attemps = thread_data[self_id]->steal_attempts;

    if (attemps >= thread_data.size())
    {
        std::this_thread::sleep_for(std::chrono::microseconds(1 << attemps));
        attemps = 0;
        return false;
    }

    if (global_queue.try_pop(task))
    {
        attemps = 0;
        return true;
    }

    // generate random number by thread_local's generator don't need synchronization
    size_t victim1 = random_data.dist(random_data.gen) % thread_data.size();
    size_t victim2 = random_data.dist(random_data.gen) % thread_data.size();

    // avoid self stealing
    victim1 = (victim1 == self_id) ? ((victim1 + 1) % thread_data.size()) : victim1;
    victim2 = (victim2 == self_id) ? ((victim2 + 1) % thread_data.size()) : victim2;

    size_t victim = victim1;
    if (thread_data[victim2]->local_queue->size() >
        thread_data[victim1]->local_queue->size())
    {
        victim = victim2;
    }

    if (thread_data[victim]->local_queue->try_pop(task))
    {
        attemps = 0;
        return true;
    }

    attemps++;
    return false;
}

void ThreadPool::worker_thread(size_t id)
{
    pthread_setname_np(pthread_self(), ("worker-" + std::to_string(id)).c_str());
    if (!setThreadAffinity(pthread_self(), id))
    {
        Logger::getInstance()->warning(
            "thread affinity setting failed for worker-" + std::to_string(id));
    }

    auto &local_data = thread_data[id];
    std::function<void()> task;
    size_t spin_count = 0;
    while (!stop_flag.load(std::memory_order_relaxed))
    {
        bool got_task = false;

        if (local_data->local_queue->try_pop(task))
        {
            got_task = true;
        }
        else if (steal_task(task, id))
        {
            got_task = true;
        }
        else if (spin_count < SPIN_COUNT_MAX)
        {
            spin_count++;
            std::this_thread::yield();
            continue;
        }
        else
        {
            std::shared_lock<std::shared_mutex> lock(taskMutex);
            condition.wait_for(
                lock,
                std::chrono::milliseconds(100),
                [this]
                {
                    return stop_flag.load(std::memory_order_relaxed) ||
                           global_queue.size() > 0;
                });

            if (stop_flag.load(std::memory_order_relaxed) &&
                global_queue.size() == 0)
            {
                break;
            }

            if (global_queue.size() > 0)
            {
                lock.unlock();
                std::unique_lock<std::shared_mutex> uniqueLock(taskMutex);
                if (global_queue.size() > 0 && global_queue.try_pop(task))
                {
                    got_task = true;
                }
            }
        }

        if (got_task)
        {
            spin_count = 0;
            active_threads.fetch_add(1, std::memory_order_relaxed);

            try
            {
                // execute task with thread local memory pool available
                task();
                local_data->tasks_processed.fetch_add(1, std::memory_order_relaxed);
            }
            catch (const std::exception &e)
            {
                Logger::getInstance()->error(
                    "thread pool task exception: " + std::string(e.what()));
            }
            catch (...)
            {
                Logger::getInstance()->error(
                    "unknown exception in thread pool task");
            }

            active_threads.fetch_sub(1, std::memory_order_relaxed);
        }
        else
        {
            spin_count = 0;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
}

bool ThreadPool::setThreadAffinity(pthread_t thread, size_t thread_id)
{
    const int num_cores = std::thread::hardware_concurrency();
    if (num_cores == 0)
    {
        return false;
    }

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);

    const int cores_per_thread = std::max(1, num_cores / 4);
    const int base_core = (thread_id * cores_per_thread) % num_cores;

    for (int i = 0; i < cores_per_thread; ++i)
    {
        CPU_SET((base_core + i) % num_cores, &cpuset);
    }

    int rc = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
    if (rc != 0)
    {
        Logger::getInstance()->warning(
            "Thread affinity setting failed: " + std::string(strerror(rc)));
        return false;
    }
    return true;
}

void ThreadPool::stop()
{
    {
        std::unique_lock<std::shared_mutex> lock(taskMutex);
        stop_flag = true;
    }
    condition.notify_all();

    for (std::thread &worker : workers)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    std::function<void()> task;
    while (global_queue.try_pop(task))
    {
    }
    for (const auto &data : thread_data)
    {
        while (data->local_queue->try_pop(task))
        {
        }
    }

    workers.clear();
    thread_data.clear();
}

ThreadPool::~ThreadPool()
{
    stop();
}