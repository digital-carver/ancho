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
  WorkerThreadFunc(TaskQueue &aQueue, boost::condition_variable &aCondVariable, boost::mutex &aMutex)
    : mQueue(aQueue), mCondVariable(aCondVariable), mMutex(aMutex)
  { /*empty*/ }

  TaskQueue &mQueue;
  boost::condition_variable &mCondVariable;
  boost::mutex &mMutex;

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
              ATLTRACE(L"ASYNC TASK MANAGER - Waiting\n");
              mCondVariable.wait(lock);
              ATLTRACE(L"ASYNC TASK MANAGER - Finished waiting\n");
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
        #pragma warning(suppress : 4101) //unreferenced variable e
        catch (std::exception &e) {
          ATLTRACE(L"ASYNC TASK MANAGER - Caught an exception: %s\n", e.what());
        }
      }
    } catch (boost::thread_interrupted &) {
      //Thread execution was properly ended.
      ATLTRACE(L"ASYNC TASK MANAGER - Worker thread interrupted\n");
      return;
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

  boost::thread mWorkerThread; //TODO - used thread_group after testing to increase parallelism
};


AsynchronousTaskManager::AsynchronousTaskManager(): mPimpl(new AsynchronousTaskManager::Pimpl())
{
  mPimpl->mWorkerThread = boost::thread(detail::WorkerThreadFunc(mPimpl->mQueue, mPimpl->mCondVariable, mPimpl->mMutex));
  mPimpl->mFinalized = false;
}

AsynchronousTaskManager::~AsynchronousTaskManager()
{
  if (mPimpl->mWorkerThread.joinable()) {
    mPimpl->mWorkerThread.interrupt();

    //wait till it finishes
    if (!mPimpl->mWorkerThread.try_join_for(boost::chrono::seconds(3))) {
      mPimpl->mWorkerThread.interrupt();
      mPimpl->mWorkerThread.join();
      //mPimpl->mWorkerThread.detach();
    }
  }
}

void AsynchronousTaskManager::addPackagedTask(boost::function<void()> aTask)
{
  {//synchronize only queue management
    boost::unique_lock<boost::mutex> lock(mPimpl->mMutex);
    if (mPimpl->mFinalized) {
      ANCHO_THROW(EFail());
    }
    mPimpl->mQueue.push_back(aTask);
  }

  //notify the threads waiting on the condition variable (threads are waiting only in case of empty queue)
  mPimpl->mCondVariable.notify_one();
}

void AsynchronousTaskManager::finalize()
{
  {//synchronize only queue management
    boost::unique_lock<boost::mutex> lock(mPimpl->mMutex);
    mPimpl->mFinalized = true;
    mPimpl->mQueue.clear();
  }
  mPimpl->mWorkerThread.interrupt();
  mPimpl->mWorkerThread.try_join_for(boost::chrono::seconds(1));
}

} //namespace Utils
} //namespace Ancho
