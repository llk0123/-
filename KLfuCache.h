#pragma once

#include <cmath> // 数学函数库
#include <memory> // 包含智能指针
#include <mutex> // 包含互斥锁，用于线程安全
#include <thread> // 包含线程相关功能
#include <unordered_map>
#include <vector>

#include "KICachePolicy.h"

namespace KamaCache
{

template<typename Key, typename Value> class KLfuCache; // 前向声明KLfuCache类，因为FreqList需要将其声明为友元

template<typename Key, typename Value>
class FreqList // 定义频率链表类，用于管理具有相同访问频率的节点集合
{
private:
    struct Node
    {
        int freq; // 访问频次
        Key key;
        Value value;
        std::weak_ptr<Node> pre; // 上一结点改为weak_ptr打破循环引用
        std::shared_ptr<Node> next;

        Node() : freq(1), next(nullptr) {} // freq: 该节点的访问频率，默认为1
        Node(Key key, Value value) : freq(1), key(key), value(value), next(nullptr) {}
    };

    using NodePtr = std::shared_ptr<Node>; // NodePtr: 节点共享指针的类型别名
    int freq_; // 访问频率
    NodePtr head_; // 假头结点
    NodePtr tail_; // 假尾结点

public:
    explicit FreqList(int n) // 构造函数    explicit防止隐式转换
     : freq_(n) 
    {
      head_ = std::make_shared<Node>();
      tail_ = std::make_shared<Node>();
      head_->next = tail_;
      tail_->pre = head_; // 这里是weak_ptr赋值
    }

    bool isEmpty() const // 判断链表是否为空：头节点的下一个节点是尾节点即为空
    {
      return head_->next == tail_;
    }

    // 提那家结点管理方法
    void addNode(NodePtr node) // 添加节点到链表尾部
    {
        if (!node || !head_ || !tail_) // 空指针检查
            return;

        node->pre = tail_->pre;
        node->next = tail_;
        tail_->pre.lock()->next = node; // 使用lock()获取shared_ptr
        tail_->pre = node;
    }

    void removeNode(NodePtr node) // 从链表中移除节点
    {
        if (!node || !head_ || !tail_)
            return;
        if (node->pre.expired() || !node->next) // 检查前驱节点是否存活且后继节点存在
            return;

        auto pre = node->pre.lock(); // 使用lock()获取shared_ptr        auto推导出的实际类型：std::shared_ptr<Node>
        pre->next = node->next;
        node->next->pre = pre;
        node->next = nullptr; // 确保显式置空next指针，彻底断开节点与链表的连接
    }

    NodePtr getFirstNode() const { return head_->next; } // 获取链表中的第一个有效节点（虚拟头节点的下一个）
    
    friend class KLfuCache<Key, Value>; // 将KLfuCache声明为友元类，允许其直接访问FreqList的私有成员
};

template <typename Key, typename Value>
class KLfuCache : public KICachePolicy<Key, Value> // 定义LFU缓存类，继承自缓存策略接口
{
public:
    using Node = typename FreqList<Key, Value>::Node; // Node: 频率链表的节点类型
    using NodePtr = std::shared_ptr<Node>; // NodePtr: 节点的共享指针
    using NodeMap = std::unordered_map<Key, NodePtr>; // NodeMap: 从key到节点的哈希映射
    // 构造函数     INT8_MAX：127   当第一个节点加入时（频率为1），updateMinFreq()会比较发现1 < 127，将minFreq_更新为1。
    KLfuCache(int capacity, int maxAverageNum = 1000000) : capacity_(capacity), minFreq_(INT8_MAX), maxAverageNum_(maxAverageNum), curAverageNum_(0), curTotalNum_(0) {}

    ~KLfuCache() override = default; // 默认析构函数

    void put(Key key, Value value) override
    {
        if (capacity_ == 0)
            return;

        std::lock_guard<std::mutex> lock(mutex_); // 加锁保证线程安全
        auto it = nodeMap_.find(key);
        if (it != nodeMap_.end())
        {
            // 如果key已存在：更新value值
            it->second->value = value;
            // 找到了直接调整就好了，不用再去get中再找一遍，但其实影响不大      调用getInternal增加访问频率
            getInternal(it->second, value); // 这里传入value其实有点多余？  其实getInternal这样设计是为了给后面的get函数，通过引入传入value
            return;
        }

        putInternal(key, value); // 如果key不存在：调用putInternal添加新节点
    }

    // value值为传出参数
    bool get(Key key, Value& value) override // 带引用参数的get方法
    {
      std::lock_guard<std::mutex> lock(mutex_); // 加锁查找
      auto it = nodeMap_.find(key);
      if (it != nodeMap_.end())
      {
          getInternal(it->second, value); // 如果找到：调用getInternal增加频率并通过引用返回value，返回true
          return true;
      }

      return false; // 如果没找到：返回false
    }

    Value get(Key key) override // 按值返回的get方法
    {
      Value value;
      get(key, value);
      return value; // 同样存在无法区分"未命中"和"命中但值为默认值"的问题 !!!!
    }

    void purge() // 清空缓存,回收资源
    {
      nodeMap_.clear(); // 清空节点映射
      freqToFreqList_.clear(); // 清空频率到链表的映射
    } // 问题：只清空了map，但没有释放FreqList对象的内存（因为freqToFreqList_存储的是原始指针）  ??????????   会不会在.cpp文件调用析构函数？

private:
    void putInternal(Key key, Value value); // 添加缓存
    void getInternal(NodePtr node, Value& value); // 获取缓存

    void kickOut(); // 移除缓存中的过期数据

    void removeFromFreqList(NodePtr node); // 从频率列表中移除节点
    void addToFreqList(NodePtr node); // 添加到频率列表

    void addFreqNum(); // 增加平均访问等频率
    void decreaseFreqNum(int num); // 减少平均访问等频率
    void handleOverMaxAverageNum(); // 处理当前平均访问频率超过上限的情况
    void updateMinFreq();

private:
    int                                            capacity_; // 缓存容量
    int                                            minFreq_; // 最小访问频次(用于找到最小访问频次结点)
    int                                            maxAverageNum_; // 最大平均访问频次
    int                                            curAverageNum_; // 当前平均访问频次
    int                                            curTotalNum_; // 当前访问所有缓存次数总数 
    std::mutex                                     mutex_; // 互斥锁
    NodeMap                                        nodeMap_; // key 到 缓存节点的映射
    std::unordered_map<int, FreqList<Key, Value>*> freqToFreqList_;// 访问频次到该频次链表的映射
};

template<typename Key, typename Value>
void KLfuCache<Key, Value>::getInternal(NodePtr node, Value& value) // 内部获取方法，当节点命中时调用，负责更新频率和返回值
{
    // 找到之后需要将其从低访问频次的链表中删除，并且添加到+1的访问频次链表中，
    // 访问频次+1, 然后把value值返回
    value = node->value;
    // 从原有访问频次的链表中删除节点
    removeFromFreqList(node); 
    node->freq++;
    addToFreqList(node); // 将节点添加到新的（频率+1）的链表中
    // 如果当前node的访问频次等于minFreq+1，并且其前驱链表为空，则说明
    // freqToFreqList_[node->freq - 1]链表因node的迁移已经空了，需要更新最小访问频次
    if (node->freq - 1 == minFreq_ && freqToFreqList_[node->freq - 1]->isEmpty())
        minFreq_++;

    // 总访问频次和当前平均访问频次都随之增加
    addFreqNum();
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::putInternal(Key key, Value value) // 内部添加方法，处理新key的插入
{   
    // 如果不在缓存中，则需要判断缓存是否已满
    if (nodeMap_.size() == capacity_)
    {
        // 缓存已满，删除最不常访问的结点，更新当前平均访问频次和总访问频次
        kickOut();
    }
    
    // 创建新结点，将新结点添加进入，更新最小访问频次
    NodePtr node = std::make_shared<Node>(key, value);
    nodeMap_[key] = node;
    addToFreqList(node);
    addFreqNum();
    minFreq_ = std::min(minFreq_, 1);
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::kickOut()
{
    NodePtr node = freqToFreqList_[minFreq_]->getFirstNode(); // 从最小频率链表中获取第一个节点（最久未访问的节点）   LFU淘汰策略：淘汰频率最小的节点中最早被访问的那个
    removeFromFreqList(node); // 从频率链表中移除该节点
    nodeMap_.erase(node->key); // 从哈希映射中删除该节点
    decreaseFreqNum(node->freq); // 更新统计信息，减去被淘汰节点的频率贡献
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::removeFromFreqList(NodePtr node)
{
    // 检查结点是否为空
    if (!node) return;
    
    auto freq = node->freq; // 获取节点当前频率
    freqToFreqList_[freq]->removeNode(node); // 从对应的频率链表中移除节点
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::addToFreqList(NodePtr node)
{
    // 检查结点是否为空
    if (!node) 
        return;

    // 添加进入相应的频次链表前需要判断该频次链表是否存在
    auto freq = node->freq;
    if (freqToFreqList_.find(freq) == freqToFreqList_.end())
    {
        // 不存在则创建
        freqToFreqList_[freq] = new FreqList<Key, Value>(freq); // 问题：使用new分配堆内存，但没有对应的delete，存在内存泄漏风险  !!!!!!
    }

    freqToFreqList_[freq]->addNode(node); // 将节点添加到对应的频率链表中
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::addFreqNum() // 管理总的访问次数
{
    curTotalNum_++; // 总访问次数加1
    if (nodeMap_.empty())
        curAverageNum_ = 0;
    else
        curAverageNum_ = curTotalNum_ / nodeMap_.size(); // 计算平均访问频率

    if (curAverageNum_ > maxAverageNum_)
    {
       handleOverMaxAverageNum(); // 如果平均频率超过阈值，触发频率衰减处理
    }
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::decreaseFreqNum(int num)
{
    // 减少平均访问频次和总访问频次
    curTotalNum_ -= num;
    if (nodeMap_.empty())
        curAverageNum_ = 0;
    else
        curAverageNum_ = curTotalNum_ / nodeMap_.size();
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::handleOverMaxAverageNum()
{
    if (nodeMap_.empty())
        return;

    // 当前平均访问频次已经超过了最大平均访问频次，所有结点的访问频次- (maxAverageNum_ / 2)
    for (auto it = nodeMap_.begin(); it != nodeMap_.end(); ++it)
    {
        // 检查结点是否为空
        if (!it->second)
            continue;

        NodePtr node = it->second;

        // 先从当前频率列表中移除
        removeFromFreqList(node);

        // 减少频率
        int oldFreq = node->freq;

        int decay = maxAverageNum_ / 2; // 衰减量：最大平均频率的一半
        node->freq -= decay;

        if (node->freq < 1)
            node->freq = 1;

        int delta = node->freq - oldFreq; // delta小于等于0
        curTotalNum_ += delta;

        // 添加到新的频率列表
        addToFreqList(node);
    }

    // 更新最小频率
    updateMinFreq();
}

template<typename Key, typename Value>
void KLfuCache<Key, Value>::updateMinFreq() // 更新全局最小频率值的方法
{
    minFreq_ = INT8_MAX; // 先将minFreq_重置为最大值（127）
    for (const auto& pair : freqToFreqList_) // 遍历所有频率链表映射
    {
        if (pair.second && !pair.second->isEmpty()) // 检查：链表指针不为空，且链表不为空    只有非空的频率链表才参与最小值的计算
        {
            minFreq_ = std::min(minFreq_, pair.first);
        }
    }
    if (minFreq_ == INT8_MAX) // 如果遍历完后minFreq_仍然是INT8_MAX，说明所有链表都为空
        minFreq_ = 1; // 此时将minFreq_设为1作为默认值
}

// 并没有牺牲空间换时间，他是把原有缓存大小进行了分片。
template<typename Key, typename Value>
class KHashLfuCache // 定义哈希分片LFU缓存类
{
public:
    KHashLfuCache(size_t capacity, int sliceNum, int maxAverageNum = 10)
        : sliceNum_(sliceNum > 0 ? sliceNum : std::thread::hardware_concurrency())
        , capacity_(capacity)
    {
        size_t sliceSize = std::ceil(capacity_ / static_cast<double>(sliceNum_)); // 每个分片的容量：总容量除以分片数，向上取整  使用static_cast<double>避免整数除法截断
        for (int i = 0; i < sliceNum_; ++i) // 循环创建每个分片的LFU缓存实例
        {
            lfuSliceCaches_.emplace_back(new KLfuCache<Key, Value>(sliceSize, maxAverageNum)); // 使用emplace_back直接在vector末尾构造unique_ptr
        }
    }

    void put(Key key, Value value)
    {
        // 根据key找出对应的lfu分片
        size_t sliceIndex = Hash(key) % sliceNum_; // 计算key的哈希值，取模得到分片索引
        lfuSliceCaches_[sliceIndex]->put(key, value); // 调用对应分片的put方法
    }

    bool get(Key key, Value& value)
    {
        // 根据key找出对应的lfu分片
        size_t sliceIndex = Hash(key) % sliceNum_;
        return lfuSliceCaches_[sliceIndex]->get(key, value); // 返回是否命中
    }

    Value get(Key key)
    {
        Value value;
        get(key, value); // 调用带引用的get方法
        return value;
    }

    // 清除缓存
    void purge()
    {
        for (auto& lfuSliceCache : lfuSliceCaches_) // 遍历所有分片，调用每个分片的purge方法
        {
            lfuSliceCache->purge();
        }
    }

private:
    // 将key计算成对应哈希值
    size_t Hash(Key key)
    {
        std::hash<Key> hashFunc; // 使用std::hash标准库提供的哈希函数
        return hashFunc(key);
    }

private:
    size_t capacity_; // 缓存总容量
    int sliceNum_; // 缓存分片数量
    std::vector<std::unique_ptr<KLfuCache<Key, Value>>> lfuSliceCaches_; // 缓存lfu分片容器
};

} // namespace KamaCache

