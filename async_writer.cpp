#include "async_writer.h"
#include <fstream>
#include <iostream>
#include <opencv2/opencv.hpp>

using namespace std;

AsyncImageWriter::AsyncImageWriter(size_t maxQueue, int pngCompression)
	: maxQueue_(maxQueue), pngCompression_(pngCompression), thread_(&AsyncImageWriter::Run, this) {}

AsyncImageWriter::~AsyncImageWriter() { Shutdown(); }

bool AsyncImageWriter::Enqueue(SaveJob &&job)
{
	{
		std::lock_guard<std::mutex> lk(m_);
		if (q_.size() >= maxQueue_)
			return false;
		q_.push_back(std::move(job));
	}
	cv_.notify_one();
	return true;
}

bool AsyncImageWriter::IsPending(const fs::path &p)
{
	std::lock_guard<std::mutex> lk(m_);
	if (hasInflight_ && inflight_ == p)
		return true;
	for (const auto &j : q_)
		if (j.path == p)
			return true;
	return false;
}

AsyncImageWriter::CancelResult AsyncImageWriter::Cancel(const fs::path &p)
{
	std::lock_guard<std::mutex> lk(m_);
	for (auto it = q_.begin(); it != q_.end(); ++it)
	{
		if (it->path == p)
		{
			q_.erase(it);
			return CancelResult::RemovedFromQueue;
		}
	}
	if (hasInflight_ && inflight_ == p)
	{
		inflightCancelled_ = true;
		return CancelResult::MarkedInFlight;
	}
	return CancelResult::NotPending;
}

std::vector<WriteResult> AsyncImageWriter::TakeResults()
{
	std::lock_guard<std::mutex> lk(m_);
	std::vector<WriteResult> out;
	out.swap(results_);
	return out;
}

size_t AsyncImageWriter::PendingCount()
{
	std::lock_guard<std::mutex> lk(m_);
	return q_.size() + (hasInflight_ ? 1 : 0);
}

void AsyncImageWriter::Shutdown()
{
	if (!thread_.joinable())
		return;
	const size_t n = PendingCount();
	if (n > 0)
		cout << "Flushing " << n << " pending image(s) to disk..." << endl;
	{
		std::lock_guard<std::mutex> lk(m_);
		stop_ = true;
	}
	cv_.notify_all();
	thread_.join();
	if (failed_ > 0)
		cout << "Warning: " << failed_ << " image(s) failed to write." << endl;
}

void AsyncImageWriter::Run()
{
	for (;;)
	{
		SaveJob job;
		{
			std::unique_lock<std::mutex> lk(m_);
			cv_.wait(lk, [&]
					 { return stop_ || !q_.empty(); });
			if (q_.empty())
				return; // stop_ 且队列已空
			job = std::move(q_.front());
			q_.pop_front();
			inflight_ = job.path;
			hasInflight_ = true;
			inflightCancelled_ = false;
		}

		std::string err;
		const bool ok = WriteImage(job, err);

		{
			std::lock_guard<std::mutex> lk(m_);
			if (inflightCancelled_)
			{
				// 写盘期间用户按了退格：写完后删除；只有删除失败才需要通知主线程
				if (ok)
				{
					std::error_code ec;
					const bool removed = fs::remove(job.path, ec);
					if (!removed || ec)
					{
						WriteResult r;
						r.id = job.id;
						r.ok = false;
						r.isDelete = true;
						r.reason = "remove failed";
						results_.push_back(r);
					}
				}
			}
			else
			{
				WriteResult r;
				r.id = job.id;
				r.ok = ok;
				r.reason = err;
				results_.push_back(r);
			}
			hasInflight_ = false;
		}
	}
}

// imencode 到内存 + ofstream(fs::path) 写文件：绕开 cv::imwrite 的非 ASCII 路径问题
bool AsyncImageWriter::WriteImage(const SaveJob &job, std::string &err)
{
	try
	{
		std::vector<uchar> buf;
		const std::vector<int> params = {cv::IMWRITE_PNG_COMPRESSION, pngCompression_};
		if (!cv::imencode(".png", job.image, buf, params))
		{
			cout << "Failed to encode: " << PathToUtf8(job.path) << endl;
			err = "encode failed";
			++failed_;
			return false;
		}

		// 先写 .part 再改名，避免程序被强杀时留下半个 PNG
		fs::path tmp = job.path;
		tmp += ".part";
		{
			std::ofstream ofs(tmp, std::ios::binary | std::ios::trunc);
			if (!ofs)
			{
				cout << "Failed to open for writing: " << PathToUtf8(tmp) << endl;
				err = "cannot open file";
				++failed_;
				return false;
			}
			ofs.write(reinterpret_cast<const char *>(buf.data()), static_cast<std::streamsize>(buf.size()));
			ofs.close();
			if (!ofs)
			{
				cout << "Failed to write: " << PathToUtf8(tmp) << endl;
				err = "write failed (disk full?)";
				std::error_code ec;
				fs::remove(tmp, ec);
				++failed_;
				return false;
			}
		}

		std::error_code ec;
		if (fs::exists(job.path, ec)) // 最后一道保险：绝不覆盖
		{
			cout << "Refuse to overwrite existing file: " << PathToUtf8(job.path) << endl;
			err = "file exists";
			fs::remove(tmp, ec);
			++failed_;
			return false;
		}
		fs::rename(tmp, job.path, ec);
		if (ec)
		{
			cout << "Failed to rename " << PathToUtf8(tmp) << ": " << ec.message() << endl;
			err = "rename failed";
			std::error_code ec2;
			fs::remove(tmp, ec2);
			++failed_;
			return false;
		}
		return true;
	}
	catch (const std::exception &e)
	{
		cout << "Writer error: " << e.what() << endl;
		err = "exception";
		++failed_;
		return false;
	}
}
