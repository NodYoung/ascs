/*
 * container.h
 *
 *  Created on: 2016-10-10
 *      Author: youngwolf
 *		email: mail2tao@163.com
 *		QQ: 676218192
 *		Community on QQ: 198941541
 *
 * containers.
 */

#ifndef _ASCS_CONTAINER_H_
#define _ASCS_CONTAINER_H_

#include "base.h"

namespace ascs
{

class dummy_lockable
{
public:
	typedef std::lock_guard<dummy_lockable> lock_guard;

	//lockable, dummy
	bool is_lockable() const {return false;}
	void lock() const {}
	void unlock() const {}
};

class lockable
{
public:
	typedef std::lock_guard<lockable> lock_guard;

	//lockable
	bool is_lockable() const {return true;}
	void lock() {mutex.lock();}
	void unlock() {mutex.unlock();}

private:
	std::mutex mutex; //std::mutex is more efficient than std::shared_(timed_)mutex
};

//Container must at least has the following functions (like std::forward_list):
// Container() and Container(size_t) constructor
// size, must be thread safe, but doesn't have to be consistent
// empty, must be thread safe, but doesn't have to be consistent
// clear
// swap
// template<typename T> emplace_back(const T& item), if you call direct_(sync_)send_msg which accepts other than rvalue reference
// template<typename T> emplace_back(T&& item)
// splice_after(Container&)
// splice_after_until(Container&, iter)
// front
// pop_front
// back
// begin
// end
template<typename Container, typename Lockable> //thread safety depends on Container or Lockable
class queue : private Container, public Lockable
{
public:
	using typename Container::value_type;
	using typename Container::size_type;
	using typename Container::reference;
	using typename Container::const_reference;
	using Container::size;
	using Container::empty;

	queue() : buff_size(0) {}
	queue(size_t capacity) : Container(capacity), buff_size(0) {}

	//thread safe
	bool is_thread_safe() const {return Lockable::is_lockable();}
	size_t size_in_byte() const {return buff_size;}
	void clear() {typename Lockable::lock_guard lock(*this); Container::clear(); buff_size = 0;}
	void swap(Container& can)
	{
		auto size_in_byte = ascs::get_size_in_byte(can);

		typename Lockable::lock_guard lock(*this);
		Container::swap(can);
		buff_size = size_in_byte;
	}

	template<typename T> bool enqueue(T&& item) {typename Lockable::lock_guard lock(*this); return enqueue_(std::forward<T>(item));}
	void move_items_in(Container& src, size_t size_in_byte = 0) {typename Lockable::lock_guard lock(*this); move_items_in_(src, size_in_byte);}
	bool try_dequeue(reference item) {typename Lockable::lock_guard lock(*this); return try_dequeue_(item);}
	void move_items_out(Container& dest, size_t max_item_num = -1) {typename Lockable::lock_guard lock(*this); move_items_out_(dest, max_item_num);}
	void move_items_out(size_t max_size_in_byte, Container& dest) {typename Lockable::lock_guard lock(*this); move_items_out_(max_size_in_byte, dest);}
	template<typename _Predicate> void do_something_to_all(const _Predicate& __pred) {typename Lockable::lock_guard lock(*this); do_something_to_all_(__pred);}
	template<typename _Predicate> void do_something_to_one(const _Predicate& __pred) {typename Lockable::lock_guard lock(*this); do_something_to_one_(__pred);}
	//thread safe

	//not thread safe
	template<typename T> bool enqueue_(T&& item)
	{
		try
		{
			auto s = item.size();
			this->emplace_back(std::forward<T>(item));
			buff_size += s;
		}
		catch (const std::exception& e)
		{
			unified_out::error_out("cannot hold more objects (%s)", e.what());
			return false;
		}

		return true;
	}

	void move_items_in_(Container& src, size_t size_in_byte = 0)
	{
		if (0 == size_in_byte)
			size_in_byte = ascs::get_size_in_byte(src);

		this->splice_after(src);
		buff_size += size_in_byte;
	}

	bool try_dequeue_(reference item) {if (this->empty()) return false; item.swap(this->front()); this->pop_front(); buff_size -= item.size(); return true;}

	void move_items_out_(Container& dest, size_t max_item_num = -1)
	{
		if ((size_t) -1 == max_item_num)
		{
			dest.splice_after(*this);
			buff_size = 0;
		}
		else if (max_item_num > 0)
		{
			size_t s = 0, index = 0;
			auto end_iter = this->begin();
			do_something_to_one_([&](const_reference item) {if (++index > max_item_num) return true; s += item.size(); ++end_iter; return false;});

			if (end_iter == this->end())
				dest.splice_after(*this);
			else
				dest.splice_after_until(*this, end_iter);
			buff_size -= s;
		}
	}

	void move_items_out_(size_t max_size_in_byte, Container& dest)
	{
		if ((size_t) -1 == max_size_in_byte)
		{
			dest.splice_after(*this);
			buff_size = 0;
		}
		else
		{
			size_t s = 0;
			auto end_iter = this->begin();
			do_something_to_one_([&](const_reference item) {s += item.size(); ++end_iter; if (s >= max_size_in_byte) return true; return false;});

			if (end_iter == this->end())
				dest.splice_after(*this);
			else
				dest.splice_after_until(*this, end_iter);
			buff_size -= s;
		}
	}

	template<typename _Predicate>
	void do_something_to_all_(const _Predicate& __pred) {for (auto& item : *this) __pred(item);}
	template<typename _Predicate>
	void do_something_to_all_(const _Predicate& __pred) const {for (auto& item : *this) __pred(item);}

	template<typename _Predicate>
	void do_something_to_one_(const _Predicate& __pred) {for (auto iter = this->begin(); iter != this->end(); ++iter) if (__pred(*iter)) break;}
	template<typename _Predicate>
	void do_something_to_one_(const _Predicate& __pred) const {for (auto iter = this->begin(); iter != this->end(); ++iter) if (__pred(*iter)) break;}
	//not thread safe

private:
	size_t buff_size; //in use
};

//ascs requires that queue must take one and only one template argument
template<typename Container> using non_lock_queue = queue<Container, dummy_lockable>; //thread safety depends on Container
template<typename Container> using lock_queue = queue<Container, lockable>;

} //namespace

#endif /* _ASCS_CONTAINER_H_ */
