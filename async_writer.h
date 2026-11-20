#pragma once
// 后台写盘线程：主线程只做 clone + 入队，PNG 编码和写文件都在这里
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <opencv2/core.hpp>
#include "text_utils.h"

struct SaveJob
{
	fs::path path;
	long long id = 0;
	cv::Mat image;
};

// 后台线程完成一次写盘后的结果，由主线程取走并显示在预览画面上
struct WriteResult
{
	long long id = 0;
	bool ok = true;
	bool isDelete = false; // true = 写盘期间被用户删除，但删除文件失败
	std::string reason;	   // 失败原因（纯 ASCII，会叠加到预览画面）
};

class AsyncImageWriter
{
public:
	enum class CancelResult
	{
		RemovedFromQueue, // 还没开始写，直接从队列撤销
		MarkedInFlight,	  // 正在写，写完后自动删除
		NotPending		  // 不在队列里，调用方自己删文件
	};

	AsyncImageWriter(size_t maxQueue, int pngCompression);
	~AsyncImageWriter();

	AsyncImageWriter(const AsyncImageWriter &) = delete;
	AsyncImageWriter &operator=(const AsyncImageWriter &) = delete;

	// 队列满返回 false
	bool Enqueue(SaveJob &&job);

	// 该路径是否还在队列里或正在写（此时磁盘上可能还看不到文件）
	bool IsPending(const fs::path &p);

	CancelResult Cancel(const fs::path &p);

	// 取走后台线程已完成的结果（主线程每轮调用一次）
	std::vector<WriteResult> TakeResults();

	size_t PendingCount();

	// 等队列写完再退出
	void Shutdown();

private:
	void Run();
	bool WriteImage(const SaveJob &job, std::string &err);

	const size_t maxQueue_;
	const int pngCompression_;

	std::mutex m_;
	std::condition_variable cv_;
	std::deque<SaveJob> q_;
	std::vector<WriteResult> results_;
	bool stop_ = false;
	fs::path inflight_;
	bool hasInflight_ = false;
	bool inflightCancelled_ = false;
	std::atomic<int> failed_{0};

	std::thread thread_; // 放最后：其余成员初始化完才启动线程
};
