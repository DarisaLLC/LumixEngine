#include "tcp_filesystem.h"
#include "task.h"
#include "core/vector.h"
#include "core/string.h"
#include "core/mutex.h"
#include <cstdio>
#include "socket.h"

namespace Lux
{
	struct TCPFileSystemTask : public Task
	{
		virtual int task() LUX_OVERRIDE;
		void stop();
		void handleMessage(char* buffer, int size);

		bool m_finished;
		Mutex m_mutex;
		struct TCPFileSystemImpl* m_fs;
		Socket m_socket;
		Socket* m_work_socket;
	};


	struct TCPFileSystemImpl
	{
		struct FileItem
		{
			enum Status
			{
				LOADING,
				SUCCESS,
				FAIL
			};
			string path;
			IFileSystem::ReadCallback callback;
			void* user_data;
			char* file_data;
			int file_length;
			int uid;
			Status status;
		};

		TCPFileSystemTask* task;
		vector<FileItem*> queue;
		vector<FileItem*> loaded;
		vector<FileItem*> in_progress;
		int last_uid;
	};


	int TCPFileSystemTask::task()
	{
		char* buffer = new char[256];
		m_socket.create(10001);
		m_work_socket = m_socket.accept();

		m_finished = false;

		while(!m_finished)
		{
			m_mutex.lock();
			if(!m_fs->queue.empty())
			{
				TCPFileSystemImpl::FileItem* item = m_fs->queue.back();
				m_fs->queue.pop_back();
				m_mutex.unlock();
				
				int len = item->path.length() + 5;
				memcpy(buffer, &len, sizeof(len));				
				buffer[4] = 0;
				memcpy(buffer+5, &item->uid, sizeof(item->uid));
				
				m_work_socket->send(buffer, 9);
				m_work_socket->send(item->path.c_str(), item->path.length());

				m_fs->in_progress.push_back(item);
			}
			else
			{
				m_mutex.unlock();
			}

			char buffer2[256];
			if(m_work_socket->canReceive())
			{
				int received = m_work_socket->receive(buffer2, 5);
				handleMessage(buffer2, received);
			}
		}
		delete[] buffer;
		return 0;
	}

	void TCPFileSystemTask::handleMessage(char* buffer, int size)
	{
		if(size > 0)
		{
			if(size < 9)
			{
				if(!m_work_socket->receiveAllBytes(buffer + size, 9 - size))
					assert(false);
				size = 9;
			}
			
			int len = *(int*)buffer;
			if(buffer[4] == 0)
			{
				int uid = *(int*)(buffer + 5);
				bool found = false;
				for(int i = 0; i < m_fs->in_progress.size(); ++i)
				{
					if(m_fs->in_progress[i]->uid == uid)
					{
						if(len < 0)
						{
							m_fs->in_progress[i]->file_length = -1;
							m_fs->in_progress[i]->file_data = 0;
							m_fs->in_progress[i]->status = TCPFileSystemImpl::FileItem::FAIL;
						}
						else
						{
							m_fs->in_progress[i]->file_length = len - 5;
							m_fs->in_progress[i]->file_data = new char[len - 5];
							if(size > 9)
								memcpy(m_fs->in_progress[i]->file_data, buffer + 9, size - 9);
							m_work_socket->receiveAllBytes(m_fs->in_progress[i]->file_data + size - 9, len - size + 4); 					
							m_fs->in_progress[i]->status = TCPFileSystemImpl::FileItem::SUCCESS;
						}
						m_mutex.lock();
						m_fs->loaded.push_back(m_fs->in_progress[i]);
						m_fs->in_progress.eraseFast(i);
						m_mutex.unlock();
						found = true;
						break;
					}
					int last_err = WSAGetLastError();
					assert(found);
				}
			}
			else
			{
				assert(false);
			}
		}
	}


	void TCPFileSystem::processLoaded()
	{
		m_impl->task->m_mutex.lock();
		if(!m_impl->loaded.empty())
		{
			TCPFileSystemImpl::FileItem* item = m_impl->loaded.back();
			m_impl->loaded.pop_back();
			m_impl->task->m_mutex.lock();
			item->callback(item->user_data, item->file_data, item->file_length, item->status == TCPFileSystemImpl::FileItem::SUCCESS);
			delete item;
		}
		else
		{
			m_impl->task->m_mutex.unlock();
		}
	}


	void TCPFileSystemTask::stop()
	{
		m_finished = true;
	}


	bool TCPFileSystem::create()
	{
		Socket::init();
		m_impl = new TCPFileSystemImpl();
		m_impl->last_uid = 0;
		m_impl->task = new TCPFileSystemTask();
		m_impl->task->m_fs = m_impl;
		if(!m_impl->task->create())
		{
			m_impl->task->m_mutex.create();
			delete m_impl;
			m_impl = 0;
			return false;
		}
		if(!m_impl->task->run())
		{
			m_impl->task->destroy();
			delete m_impl;
			m_impl = 0;
			return false;
		}
		return true;
	}


	void TCPFileSystem::destroy()
	{
		if(m_impl)
		{
			m_impl->task->stop();
			m_impl->task->m_mutex.destroy();			
			m_impl->task->destroy();
			delete m_impl;
			m_impl = 0;
		}
	}


	IFileSystem::Handle TCPFileSystem::openFile(const char* path, ReadCallback callback, void* user_data)
	{
		TCPFileSystemImpl::FileItem* item = new TCPFileSystemImpl::FileItem();
		item->callback = callback;
		item->path = path;
		item->user_data = user_data;
		item->uid = ++m_impl->last_uid;
		m_impl->task->m_mutex.lock();
		m_impl->queue.push_back(item);
		m_impl->task->m_mutex.unlock();
		return item->uid;
	}


} // ~namespace Lux