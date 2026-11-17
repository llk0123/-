#pragma once 

#include <cstring> // 包含 C 风格字符串操作，但实际未使用
#include <list> // 包含但未使用，可能是预留
#include <memory> // 智能指针支持
#include <mutex> // 线程安全锁
#include <unordered_map>

#include "KICachePolicy.h" // 引入之前定义的抽象基类

namespace KamaCache
{

// 前向声明KLruCache类模板，因为LruNode需要将其声明为友元类
template<typename Key, typename Value> class KLruCache;

template<typename Key, typename Value>
class LruNode 
{
private:
    Key key_;
    Value value_;
    size_t accessCount_;  // 访问次数
    std::weak_ptr<LruNode<Key, Value>> prev_;  // 改为weak_ptr打破循环引用      避免双向链表中的循环引用导致内存泄漏
    std::shared_ptr<LruNode<Key, Value>> next_;

public:
    LruNode(Key key, Value value) // 构造函数
        : key_(key)
        , value_(value)
        , accessCount_(1) 
    {}

    // 提供必要的访问器
    Key getKey() const { return key_; } // 这里的const：该函数不会修改当前对象的任何成员变量     并且只有const对象才能调用，如果没有const则无法调用
    Value getValue() const { return value_; }
    void setValue(const Value& value) { value_ = value; }
    size_t getAccessCount() const { return accessCount_; }
    void incrementAccessCount() { ++accessCount_; }

    friend class KLruCache<Key, Value>; // 将KLruCache声明为友元类，使其可以直接访问LruNode的私有成员
};


template<typename Key, typename Value>
class KLruCache : public KICachePolicy<Key, Value>
{
public: // 类型别名定义
    using LruNodeType = LruNode<Key, Value>; // LruNodeType: 节点类型
    using NodePtr = std::shared_ptr<LruNodeType>; // NodePtr: 节点的共享指针类型
    using NodeMap = std::unordered_map<Key, NodePtr>; // NodeMap: 从Key到NodePtr的哈希映射类型

    KLruCache(int capacity) : capacity_(capacity) // 构造函数，初始化容量
    {
        initializeList(); // 创建虚拟头尾节点
    }

    ~KLruCache() override = default; // 默认析构函数，使用override关键字明确表示覆盖基类的虚析构函数

    // 添加缓存
    void put(Key key, Value value) override
    {
        if (capacity_ <= 0)
            return;
    
        std::lock_guard<std::mutex> lock(mutex_); // 加锁保证线程安全
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // 如果在当前容器中,则更新value,并调用get方法，代表该数据刚被访问
            updateExistingNode(it->second, value); // 如果存在：调用updateExistingNode更新值并移到链表头部
            return ;
        }

        addNewNode(key, value); // 如果不存在：调用addNewNode添加新节点
    }

    bool get(Key key, Value& value) override
    {
        std::lock_guard<std::mutex> lock(mutex_); // 加锁
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            moveToMostRecent(it->second); // nodeMap_的类型是：unordered_map<Key, NodePtr>       如果找到：将节点移到最近使用位置
            value = it->second->getValue(); // 通过引用返回value，搭配下面的get函数就懂了。
            return true;
        }
        return false;
    }

    Value get(Key key) override
    {
        Value value{}; // 创建一个默认构造的value
        // memset(&value, 0, sizeof(value));   // memset 是按字节设置内存的，对于复杂类型（如 string）使用 memset 可能会破坏对象的内部结构
        get(key, value); // 调用带引用的get方法
        return value; // 问题：如果缓存未命中，返回的是默认构造的空值，调用者无法区分"未命中"和"命中但值为默认值"两种情况 !!!
    }

    // 删除指定元素
    void remove(Key key) 
    {   
        std::lock_guard<std::mutex> lock(mutex_); // 加锁
        auto it = nodeMap_.find(key); // 在哈希表中查找
        if (it != nodeMap_.end())
        {
            removeNode(it->second); // 如果找到：先从链表中移除节点，再从哈希表中擦除
            nodeMap_.erase(it);
        }
    }

private:
    void initializeList()
    {
        // 创建首尾虚拟节点
        dummyHead_ = std::make_shared<LruNodeType>(Key(), Value()); // 创建虚拟头节点和虚拟尾节点（使用Key和Value的默认构造函数）
        dummyTail_ = std::make_shared<LruNodeType>(Key(), Value()); // 这里的Key()和 Value()是创建临时对象，把这两个临时对象传递给 LruNodeType的构造函数
        dummyHead_->next_ = dummyTail_;
        dummyTail_->prev_ = dummyHead_; // 注意：这里dummyTail_->prev_赋值时，dummyTail_->prev_是weak_ptr，dummyHead_是shared_ptr，赋值会隐式转换为weak_ptr
    }

    void updateExistingNode(NodePtr node, const Value& value) // 更新已有节点：设置新值并将其移到链表前端（最近使用位置）
    {
        node->setValue(value);
        moveToMostRecent(node);
    }

    void addNewNode(const Key& key, const Value& value) // 添加新节点
    {
       if (nodeMap_.size() >= capacity_) // 如果缓存已满，先驱逐最近最少使用的节点
       {
           evictLeastRecent();
       }

       NodePtr newNode = std::make_shared<LruNodeType>(key, value); // 创建新节点
       insertNode(newNode); // 插入到链表前端
       nodeMap_[key] = newNode; // 添加到哈希表
    }

    // 将该节点移动到最新的位置
    void moveToMostRecent(NodePtr node) // 将节点移到最近使用位置：先从当前位置移除，再插入到链表前端
    {
        removeNode(node);
        insertNode(node);
    }

    void removeNode(NodePtr node) // 从链表中移除节点
    { // 检查前驱节点是否仍然存在（!expired()）且后继节点不为空
        if(!node->prev_.expired() && node->next_) // weak_ptr本身没有提供直接判断是否有效的方法，它不像普通指针那样可以直接用 if (ptr)来判断
        { // weak_ptr本身不支持直接解引用（即不能通过 ->或 *访问所指向的对象）。这是 weak_ptr的设计特性，它只是一个"观察者"，不拥有对象的所有权。
            auto prev = node->prev_.lock(); // 通过lock()将weak_ptr提升为shared_ptr
            prev->next_ = node->next_;
            node->next_->prev_ = prev;
            node->next_ = nullptr; // 清空next_指针，彻底断开节点与链表的连接
        }
    }

    // 从尾部插入结点
    void insertNode(NodePtr node) 
    {
        node->next_ = dummyTail_; // 新节点的next指向虚拟尾节点
        node->prev_ = dummyTail_->prev_; // 新节点的prev指向原倒数第二个节点
        dummyTail_->prev_.lock()->next_ = node; // 使用lock()获取shared_ptr     原倒数第二个节点的next指向新节点
        dummyTail_->prev_ = node; // 虚拟尾节点的prev指向新节点
    }

    // 驱逐最近最少使用的节点
    void evictLeastRecent() 
    {
        NodePtr leastRecent = dummyHead_->next_; // 获取虚拟头节点的下一个节点（最久未使用的节点）
        removeNode(leastRecent);
        nodeMap_.erase(leastRecent->getKey());
    }

private:
    int           capacity_; // 缓存容量
    NodeMap       nodeMap_; // 哈希表：key -> Node 
    std::mutex    mutex_; // 互斥锁，保证线程安全
    NodePtr       dummyHead_; // 虚拟头结点
    NodePtr       dummyTail_; // 虚拟尾结点
};

// LRU优化：Lru-k版本。 通过继承的方式进行再优化
template<typename Key, typename Value>
class KLruKCache : public KLruCache<Key, Value> // 定义LRU-K缓存类，继承自基础的KLruCache
{
public:
    KLruKCache(int capacity, int historyCapacity, int k) // historyCapacity: 历史记录缓存容量       k: 进入主缓存的阈值
        : KLruCache<Key, Value>(capacity) // 调用基类构造函数初始化主缓存
        , historyList_(std::make_unique<KLruCache<Key, size_t>>(historyCapacity)) // 创建历史记录缓存，类型为KLruCache<Key, size_t>，即记录key的访问次数
        , k_(k) // 保存k值
    {}

    Value get(Key key) // 首先尝试从主缓存获取数据
    {
        Value value{};
        bool inMainCache = KLruCache<Key, Value>::get(key, value); // 显式调用基类的get方法，避免递归调用自身     inMainCache表示是否在主缓存中找到

        // 如果数据在主缓存中，直接返回
        if (inMainCache) // 如果主缓存命中，直接返回value
        {
            return value;
        }

        // 只有在主缓存未命中时才更新历史计数（但如果不断请求一个不存在的key，也会导致计数增长，虽然不会存入到historyValueMap_中，但如果后续再put这个key，计数也会保持未put的次数）
        size_t historyCount = historyList_->get(key); // 从历史记录中获取当前的访问次数
        historyCount++; // 递增访问次数
        historyList_->put(key, historyCount); // 将更新后的次数放回历史记录

        // 如果数据不在主缓存，但访问次数达到了k次
        if (historyCount >= k_) 
        {
            // 检查是否有历史值记录
            auto it = historyValueMap_.find(key); // 在historyValueMap_中查找之前保存的值
            if (it != historyValueMap_.end()) 
            {
                // 如果有历史值，将其添加到主缓存
                Value storedValue = it->second; // 如果找到：从历史记录中移除该key，将值加入主缓存，返回该值
                
                // 从历史记录移除
                historyList_->remove(key);
                historyValueMap_.erase(it);
                
                // 添加到主缓存
                KLruCache<Key, Value>::put(key, storedValue); // 只有put操作才能触发加入主缓存
                
                return storedValue;
            }
            // 没有历史值记录，无法添加到缓存，返回默认值
        }

        // 数据不在主缓存且不满足添加条件，返回默认值
        return value;
    }

    void put(Key key, Value value) 
    {
        // 检查是否已在主缓存
        Value existingValue{};
        bool inMainCache = KLruCache<Key, Value>::get(key, existingValue); // 先检查key是否已在主缓存
        
        if (inMainCache) 
        {
            // 如果在主缓存中，直接更新值并返回（注意：这里也会触发基类put中的LRU移动操作）
            KLruCache<Key, Value>::put(key, value);
            return;
        }
        
        // 如果不在主缓存，更新历史访问计数
        size_t historyCount = historyList_->get(key);
        historyCount++;
        historyList_->put(key, historyCount);
        
        // 保存值到历史记录映射，供后续get操作使用
        historyValueMap_[key] = value;
        
        // 检查是否达到k次访问阈值
        if (historyCount >= k_) 
        {
            // 达到阈值，添加到主缓存
            historyList_->remove(key);
            historyValueMap_.erase(key);
            KLruCache<Key, Value>::put(key, value);
        }
    }

private:
    int                                     k_; // 进入缓存队列的评判标准
    std::unique_ptr<KLruCache<Key, size_t>> historyList_; // 使用智能指针管理的LRU缓存，记录每个key的访问次数
    std::unordered_map<Key, Value>          historyValueMap_; // 存储尚未达到k次访问的key对应的值
};

// lru优化：对lru进行分片，提高高并发使用的性能
template<typename Key, typename Value>
class KHashLruCaches // 定义哈希分片LRU缓存类        通过将缓存分成多个独立的LRU实例来提高并发性能，减少锁竞争
{
public:
    KHashLruCaches(size_t capacity, int sliceNum) // 构造函数      sliceNum: 分片数量
        : capacity_(capacity)
        , sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency()) // 如果传入的分片数<=0，则使用CPU核心数作为默认值
    {
        size_t sliceSize = std::ceil(capacity / static_cast<double>(sliceNum_)); // 计算每个分片的容量：向上取整，确保总容量足够   使用static_cast<double>避免整数除法截断
        for (int i = 0; i < sliceNum_; ++i) // 循环创建每个分片的LRU缓存实例
        {
            lruSliceCaches_.emplace_back(new KLruCache<Key, Value>(sliceSize)); // 使用emplace_back直接在vector末尾构造unique_ptr
        }
    }

    void put(Key key, Value value)
    {
        // 获取key的hash值，并计算出对应的分片索引
        size_t sliceIndex = Hash(key) % sliceNum_;
        lruSliceCaches_[sliceIndex]->put(key, value); // 调用对应分片的put方法
    }

    bool get(Key key, Value& value) // 带引用参数的get方法
    {
        // 获取key的hash值，并计算出对应的分片索引
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lruSliceCaches_[sliceIndex]->get(key, value);
    }

    Value get(Key key) // 按值返回的get方法
    {
        Value value;
        //memset(&value, 0, sizeof(value)); // memset危险用法：在Value get(Key key)中使用memset清零，对于非POD类型是未定义行为
        get(key, value);
        return value;
    }

private:
    // 将key转换为对应hash值
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc; // 使用std::hash标准库提供的哈希函数
        return hashFunc(key); // 按值传递key，对于大对象可能有效率问题
    }

private:
    size_t                                              capacity_;  // 总容量
    int                                                 sliceNum_;  // 切片数量
    std::vector<std::unique_ptr<KLruCache<Key, Value>>> lruSliceCaches_; // 存储各个分片LRU缓存的vector
};

} // namespace KamaCache

//get方法未命中时更新历史计数：即使主缓存未命中，也会更新历史计数，这可能导致历史计数膨胀       ?????