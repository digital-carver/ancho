#include "stdafx.h"
#include <AnchoCommons/AsynchronousTaskManager.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/scope_exit.hpp>
#include <boost/atomic.hpp>
#include <Exceptions.h>
namespace Ancho {
namespace Utils {

namespace detail {
typedef std::deque<boost::function<void()> > TaskQueue;

/**
  Functor which is invoked in task manager's worker thread.
  Access to task queue is synchronized by condition variable.
 **/
struct WorkerThreadFunc
{
  WorkerThreadFunc(TaskQueue &aQueue, boost::condition_variable &aCondVariable, boost::mutex &aMutex, int &aAvailableWorkerCount)
    : mQueue(aQueue), mCondVariable(aCondVariable), mMutex(aMutex), mAvailableWorkerCount(aAvailableWorkerCount)
  { /*empty*/ }

  TaskQueue &mQueue;
  boost::condition_variable &mCondVariable;
  boost::mutex &mMutex;
  int &mAvailableWorkerCount;

  void operator()()
  {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    BOOST_SCOPE_EXIT_ALL(&) { CoUninitialize(); };
    try {
      while (true) {

        boost::function<void()> task;

        {//synchronize only queue management
          boost::unique_lock<boost::mutex> lock(mMutex);
          while (mQueue.empty()) {
              ++mAvailableWorkerCount;
              ATLTRACE(L"ASYNC TASK MANAGER - Waiting %d threads\n", mAvailableWorkerCount);
              mCondVariable.wait(lock);
              --mAvailableWorkerCount;
              ATLTRACE(L"ASYNC TASK MANAGER - Finished waiting - %d threads still waiting\n", mAvailableWorkerCount);
          }
          task = mQueue.front();
          mQueue.pop_front();
        }
        //Allow thread interruption before processing the task
        boost::this_thread::interruption_point();
        ATLTRACE(L"ASYNC TASK MANAGER - Starting the task\n");
        try {
          task();
          ATLTRACE(L"ASYNC TASK MANAGER - Finishing the task\n");
        } catch (boost::thread_interrupted &) {
          throw;
        }
        catch (std::exception &e) {
          (void)e;
          ATLTRACE(L"ASYNC TASK MANAGER - Caught an exception: %s\n", e.what());
        }
      }
    } catch (boost::thread_interrupted &) {
      //Thread execution was properly ended.
      ATLTRACE(L"ASYNC TASK MANAGER - Worker thread interrupted\n");
    }
  }
};
} //namespace detail



struct AsynchronousTaskManager::Pimpl
{
  boost::atomic<bool> mFinalized;
  detail::TaskQueue mQueue;
  boost::condition_variable mCondVariable;
  boost::mutex mMutex;

  std::list<boost::thread> mWorkerThreads;

  /// Maintain number of worker threads waiting for a job
  int mAvailableWorkerCount;
};


AsynchronousTaskManager::AsynchronousTaskManager(): mPimpl(new AsynchronousTaskManager::Pimpl())
{
  mPimpl->mAvailableWorkerCount = 0;
  mPimpl->mFinalized = false;

  for (size_t i = 0; i < cDefaultNumberOfWorkerThreads; ++i) {
    createNewWorkerThread();
  }
}

AsynchronousTaskManager::~AsynchronousTaskManager()
{
  finalize();
}

void AsynchronousTaskManager::addPackagedTask(boost::function<void()> aTask)
{
  {//synchronize only queue management
    boost::unique_lock<boost::mutex> lock(mPimpl->mMutex);
    if (mPimpl->mFinalized) {
      ANCHO_THROW(EFail());
    }
    // if no worker available and we didn't spawn too much threads yet
    if (mPimpl->mAvailableWorkerCount == 0 && mPimpl->mWorkerThreads.size() < cMaxNumberOfWorkerThreads) {
      createNewWorkerThread();
    }
    mPimpl->mQueue.push_back(aTask);
  }

  //notify the threads waiting on the condition variable (threads are waiting only in case of empty queue)
  mPimpl->mCondVariable.notify_one();
}

void AsynchronousTaskManager::createNewWorkerThread()
{
  ATLTRACE(L"ASYNC TASK MANAGER - Creating new worker thread - %d\n", mPimpl->mWorkerThreads.size());
  mPimpl->mWorkerThreads.push_back(
    boost::thread(detail::WorkerThreadFunc(mPimpl->mQueue, mPimpl->mCondVariable, mPimpl->mMutex, mPimpl->mAvailableWorkerCount)));
}

void AsynchronousTaskManager::finalize()
{
  if (mPimpl->mFinalized) {
    // we are already done
    return;
  }
  mPimpl->mFinalized = true;
  {//synchronize only queue management
    boost::unique_lock<boost::mutex> lock(mPimpl->mMutex);
    mPimpl->mQueue.clear();
  }

  //Inform all threads that we want them finished
  for (auto it = mPimpl->mWorkerThreads.begin(); it != mPimpl->mWorkerThreads.end(); ++it) {
    it->interrupt();
  }

  //Try to join finished threads
  for (auto it = mPimpl->mWorkerThreads.begin(); it != mPimpl->mWorkerThreads.end(); ++it) {
    if (it->joinable()) {
      if (!it->try_join_for(boost::chrono::milliseconds(50))) {
        continue;
      }
    }
    it = mPimpl->mWorkerThreads.erase(it);
    if (it == mPimpl->mWorkerThreads.end()) {
      break;
    }
  }

  //No remaining worker threads? -> We are done.
  if (mPimpl->mWorkerThreads.empty()) {
    return;
  }

  //Wait for the remaining threads and let them go in case of unsuccesfull join
  for (auto it = mPimpl->mWorkerThreads.begin(); it != mPimpl->mWorkerThreads.end(); ++it) {
    if (it->joinable()) {
      if (!it->try_join_for(boost::chrono::seconds(2))) {
        it->interrupt();
#ifndef ANCHO_DEBUG_THREAD_JOINS
        it->detach();
#else
        it->join();
#endif //ANCHO_DEBUG_THREAD_JOINS
      }
    }
    it = mPimpl->mWorkerThreads.erase(it);
    if (it == mPimpl->mWorkerThreads.end()) {
      break;
    }
  }
  ATLASSERT(mPimpl->mWorkerThreads.empty());
}

} //namespace Utils
} //namespace Ancho
