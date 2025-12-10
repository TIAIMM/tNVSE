#pragma once

typedef void* (*_FormHeap_Allocate)(UInt32 size);
extern const _FormHeap_Allocate FormHeap_Allocate;

typedef void (*_FormHeap_Free)(void* ptr);
extern const _FormHeap_Free FormHeap_Free;

enum
{
	eListCount = -3,
	eListEnd = -2,
	eListInvalid = -1,
};

template <typename T_Data>
struct ListNode
{
	T_Data* data;
	ListNode* next;

	ListNode() : data(NULL), next(NULL) {}
	ListNode(T_Data* _data) : data(_data), next(NULL) {}

	T_Data* Data() const { return data; }
	ListNode* Next() const { return next; }

	ListNode* RemoveMe()
	{
		if (next)
		{
			ListNode* pNext = next;
			data = next->data;
			next = next->next;
			FormHeap_Free(pNext);
			return this;
		}
		data = NULL;
		return NULL;
	}

	ListNode* RemoveNext()
	{
		ListNode* pNext = next;
		next = next->next;
		FormHeap_Free(pNext);
		return next;
	}

	ListNode* Append(T_Data* _data)
	{
		ListNode* newNode = (ListNode*)FormHeap_Allocate(sizeof(ListNode));
		newNode->data = _data;
		newNode->next = next;
		next = newNode;
		return newNode;
	}

	ListNode* Insert(T_Data* _data)
	{
		ListNode* newNode = (ListNode*)FormHeap_Allocate(sizeof(ListNode));
		newNode->data = data;
		data = _data;
		newNode->next = next;
		next = newNode;
		return newNode;
	}
};

template <class Item>
class tList
{
public:
	typedef ListNode<Item> _Node;

	_Node m_listHead;

	template <class Op>
	UInt32 FreeNodes(_Node* node, Op&& compareOp) const
	{
		static UInt32 nodeCount = 0, numFreed = 0, lastNumFreed = 0;
		if (node->next)
		{
			nodeCount++;
			FreeNodes(node->next, compareOp);
			nodeCount--;
		}
		if (compareOp.Accept(node->data))
		{
			node->RemoveMe();
			numFreed++;
		}
		if (!nodeCount)
		{
			lastNumFreed = numFreed;
			numFreed = 0;
		}
		return lastNumFreed;
	}

	_Node* GetLastNode(SInt32* outIdx = NULL) const
	{
		SInt32 index = 0;
		_Node* node = Head();
		while (node->next)
		{
			node = node->next;
			index++;
		}
		if (outIdx)
			*outIdx = index;
		return node;
	}

	_Node* GetNthNode(SInt32 index) const
	{
		if (index >= 0)
		{
			_Node* node = Head();
			do
			{
				if (!index)
					return node;
				index--;
			} while (node = node->next);
		}
		return NULL;
	}

	void Init(Item* item = NULL)
	{
		m_listHead.data = item;
		m_listHead.next = NULL;
	}

	_Node* Head() const { return const_cast<_Node*>(&m_listHead); }

	bool Empty() const { return !m_listHead.data; }

	class Iterator
	{
		_Node* m_curr;

	public:
		Iterator operator++()
		{
			if (m_curr)
				m_curr = m_curr->next;
			return *this;
		}
		bool End() const { return !m_curr || (!m_curr->data && !m_curr->next); }
		Item* operator->() const { return m_curr->data; }
		Item*& operator*() const { return m_curr->data; }
		const Iterator& operator=(const Iterator& rhs)
		{
			m_curr = rhs.m_curr;
			return *this;
		}
		Item* Get() const { return m_curr->data; }
		void Next()
		{
			if (m_curr)
				m_curr = m_curr->next;
		}
		void Find(Item* _item)
		{
			while (m_curr)
			{
				if (m_curr->data == _item)
					break;
				m_curr = m_curr->next;
			}

		}
		bool operator!=(const Iterator& other) const
		{
			return m_curr != other.m_curr;
		}

		bool operator!=(Iterator&& other) const
		{
			return m_curr != other.m_curr;
		}

		Iterator(_Node* node = NULL) : m_curr(node) {}
		Iterator(tList& _list) : m_curr(&_list.m_listHead) {}
		Iterator(tList* _list) : m_curr(&_list->m_listHead) {}
		Iterator(tList& _list, Item* _item) : m_curr(&_list.m_listHead) { Find(_item); }
		Iterator(tList* _list, Item* _item) : m_curr(&_list->m_listHead) { Find(_item); }
	};

	Iterator Begin() const { return Iterator(Head()); }

	UInt32 Count() const
	{
		if (!m_listHead.data)
			return 0;
		_Node* node = Head();
		UInt32 count = 1;
		while (node = node->next)
			count++;
		return count;
	};

	bool IsInList(Item* item) const
	{
		_Node* node = Head();
		do
		{
			if (node->data == item)
				return true;
			node = node->next;
		} while (node);
		return false;
	}

	Item* GetFirstItem() const
	{
		return m_listHead.data;
	}

	Item* GetLastItem() const
	{
		return GetLastNode()->data;
	}

	Item* GetNthItem(SInt32 index) const
	{
		if (eListEnd == index)
			return GetLastNode()->data;
		_Node* node = GetNthNode(index);
		return node ? node->data : NULL;
	}

	SInt32 AddAt(Item* item, SInt32 index)
	{
		if (!item)
			return eListInvalid;
		_Node* node;
		if (!index)
		{
			if (m_listHead.data)
				m_listHead.Insert(item);
			else
				m_listHead.data = item;
		}
		else if (eListEnd == index)
		{
			node = GetLastNode(&index);
			if (node->data)
				node->Append(item);
			else
				node->data = item;
		}
		else
		{
			node = GetNthNode(index);
			if (!node)
				return eListInvalid;
			node->Insert(item);
		}
		return index;
	}

	SInt32 Append(Item* item)
	{
		SInt32 index = eListInvalid;
		if (item)
		{
			_Node* node = GetLastNode(&index);
			if (node->data)
				node->Append(item);
			else
				node->data = item;
		}
		return index;
	}

	void Insert(Item* item)
	{
		if (item)
		{
			if (m_listHead.data)
				m_listHead.Insert(item);
			else
				m_listHead.data = item;
		}
	}

	void CopyFrom(tList& sourceList)
	{
		_Node* target = Head(), * source = sourceList.Head();
		RemoveAll();
		if (!source->data)
			return;
		target->data = source->data;
		while (source = source->next)
			target = target->Append(source->data);
	}

	template <class Op>
	void Visit(Op&& op, _Node* prev = NULL) const
	{
		_Node* curr = prev ? prev->next : Head();
		while (curr)
		{
			if (!curr->data || !op.Accept(curr->data))
				break;
			curr = curr->next;
		}
	}

	template <class Op>
	Item* Find(Op&& op) const
	{
		_Node* curr = Head();
		Item* pItem;
		do
		{
			pItem = curr->data;
			if (pItem && op.Accept(pItem))
				return pItem;
			curr = curr->next;
		} while (curr);
		return NULL;
	}

	template <class Op>
	Iterator Find(Op&& op, Iterator& prev) const
	{
		Iterator curIt = prev.End() ? Begin() : ++prev;
		while (!curIt.End())
		{
			if (*curIt && op.Accept(*curIt))
				break;
			++curIt;
		}
		return curIt;
	}

	template <class Op>
	UInt32 CountIf(Op&& op) const
	{
		UInt32 count = 0;
		_Node* curr = Head();
		do
		{
			if (curr->data && op.Accept(curr->data))
				count++;
			curr = curr->next;
		} while (curr);
		return count;
	}

	class AcceptAll
	{
	public:
		bool Accept(Item* item) { return true; }
	};

	void RemoveAll() const
	{
		_Node* nextNode = Head(), * currNode = nextNode->next;
		nextNode->data = NULL;
		nextNode->next = NULL;
		while (currNode)
		{
			nextNode = currNode->next;
			FormHeap_Free(currNode);
			currNode = nextNode;
		}
	}

	void DeleteAll() const
	{
		_Node* nextNode = Head(), * currNode = nextNode->next;
		FormHeap_Free(nextNode->data);
		nextNode->data = NULL;
		nextNode->next = NULL;
		while (currNode)
		{
			nextNode = currNode->next;
			currNode->data->~Item();
			FormHeap_Free(currNode->data);
			FormHeap_Free(currNode);
			currNode = nextNode;
		}
	}

	Item* RemoveNth(SInt32 idx)
	{
		Item* removed = NULL;
		if (idx <= 0)
		{
			removed = m_listHead.data;
			m_listHead.RemoveMe();
		}
		else
		{
			_Node* node = Head();
			while (node->next && --idx)
				node = node->next;
			if (!idx)
			{
				removed = node->next->data;
				node->RemoveNext();
			}
		}
		return removed;
	};

	UInt32 Remove(Item* item)
	{
		UInt32 removed = 0;
		_Node* curr = Head(), * prev = NULL;
		do
		{
			if (curr->data == item)
			{
				curr = prev ? prev->RemoveNext() : curr->RemoveMe();
				removed++;
			}
			else
			{
				prev = curr;
				curr = curr->next;
			}
		} while (curr);
		return removed;
	}

	Item* ReplaceNth(SInt32 index, Item* item)
	{
		Item* replaced = NULL;
		if (item)
		{
			_Node* node;
			if (eListEnd == index)
				node = GetLastNode();
			else
			{
				node = GetNthNode(index);
				if (!node)
					return NULL;
			}
			replaced = node->data;
			node->data = item;
		}
		return replaced;
	}

	UInt32 Replace(Item* item, Item* replace)
	{
		UInt32 replaced = 0;
		_Node* curr = Head();
		do
		{
			if (curr->data == item)
			{
				curr->data = replace;
				replaced++;
			}
			curr = curr->next;
		} while (curr);
		return replaced;
	}

	template <class Op>
	UInt32 RemoveIf(Op&& op)
	{
		return FreeNodes(Head(), op);
	}

	SInt32 GetIndexOf(Item* item)
	{
		SInt32 idx = 0;
		_Node* curr = Head();
		do
		{
			if (curr->data == item)
				return idx;
			idx++;
			curr = curr->next;
		} while (curr);
		return -1;
	}

	template <class Op>
	SInt32 GetIndexOf(Op&& op)
	{
		SInt32 idx = 0;
		_Node* curr = Head();
		do
		{
			if (curr->data && op.Accept(curr->data))
				return idx;
			idx++;
			curr = curr->next;
		} while (curr);
		return -1;
	}

	template <typename F>
	Item* FindFirst(F&& func) const
	{
		for (auto iter = Begin(); !iter.End(); ++iter)
		{
			if (*iter && func(*iter))
				return *iter;
		}
		return nullptr;
	}

	template <typename F>
	void ForEach(F&& func) const
	{
		for (auto iter = Begin(); !iter.End(); ++iter)
		{
			if (*iter)
				func(*iter);
		}
	}

	template <typename F>
	bool Contains(F&& func) const
	{
		return FindFirst(func) != nullptr;
	}

	// for use with C++11 range based loops only.
	Iterator begin() const
	{
		return Begin();
	}

	// for use with C++11 range based loops only.
	Iterator end() const
	{
		return Iterator(static_cast<_Node*>(nullptr));
	}

};
STATIC_ASSERT(sizeof(tList<void*>) == 0x8);