#pragma once

#include "KArcCacheNode.h"
#include <unordered_map>
#include <map>
#include <mutex>

namespace KamaCache 
{

template<typename Key, typename Value>
class ArcLfuPart 
{
public:
    using NodeType = ArcNode<Key, Value>; // ARC节点类型
    using NodePtr = std::shared_ptr<NodeType>; // 节点的共享指针
    using NodeMap = std::unordered_map<Key, NodePtr>; // 从key到节点的哈希映射
    using FreqMap = std::map<size_t, std::list<NodePtr>>; // 从频率值到节点列表的有序映射（std::map按键升序排列）

    explicit ArcLfuPart(size_t capacity, size_t transformThreshold) // 构造函数
        : capacity_(capacity)
        , ghostCapacity_(capacity)
        , transformThreshold_(transformThreshold) // 转换阈值（此处似乎未在LFU部分使用，可能为ARC整体协调用）
        , minFreq_(0) // 初始化为0，表示当前最小频率
    {
        initializeLists(); // 初始化幽灵链表
    }

    bool put(Key key, Value value) 
    {
        if (capacity_ == 0) 
            return false;

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) // 如果key已存在：更新值并增加频率
        {
            return updateExistingNode(it->second, value);
        }
        return addNewNode(key, value); // 如果key不存在：添加新节点
    }

    bool get(Key key, Value& value) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) // 如果找到：更新节点频率，通过引用返回value，返回true
        {
            updateNodeFrequency(it->second);
            value = it->second->getValue();
            return true;
        }
        return false; // 如果没找到：返回false
    }

    bool contain(Key key) // 检查key是否存在于主缓存中
    {
        return mainCache_.find(key) != mainCache_.end();
    }

    bool checkGhost(Key key) // 检查幽灵缓存
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) // 如果key在幽灵缓存中，移除并返回true
        {
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

    void increaseCapacity() { ++capacity_; } // 增加LFU部分的容量
    
    bool decreaseCapacity() // 减少LFU部分的容量
    {
        if (capacity_ <= 0) return false;
        if (mainCache_.size() == capacity_) // 如果容量已满，先淘汰一个元素
        {
            evictLeastFrequent();
        }
        --capacity_; // 容量减 1
        return true;
    }

private:
    void initializeLists() // 初始化幽灵链表
    {
        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    bool updateExistingNode(NodePtr node, const Value& value) // 更新已有节点：设置新值并增加频率
    {
        node->setValue(value);
        updateNodeFrequency(node);
        return true;
    }

    bool addNewNode(const Key& key, const Value& value) // 添加新节点
    {
        if (mainCache_.size() >= capacity_) // 如果缓存已满，淘汰最不常用的节点
        {
            evictLeastFrequent();
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value); // 创建新节点
        mainCache_[key] = newNode; // 加入哈希映射
        
        // 将新节点添加到频率为1的列表中
        if (freqMap_.find(1) == freqMap_.end()) 
        {
            freqMap_[1] = std::list<NodePtr>();
        }
        freqMap_[1].push_back(newNode);
        minFreq_ = 1;
        
        return true;
    }

    void updateNodeFrequency(NodePtr node) // 更新节点频率
    {
        size_t oldFreq = node->getAccessCount(); // 获取旧频率
        node->incrementAccessCount(); // 频率加1
        size_t newFreq = node->getAccessCount(); // 获取新频率

        // 从旧频率列表中移除
        auto& oldList = freqMap_[oldFreq]; // 使用std::list::remove按值移除（线性搜索，效率较低）
        oldList.remove(node);
        if (oldList.empty()) // 如果旧频率列表为空，从freqMap_中删除该频率项
        {
            freqMap_.erase(oldFreq);
            if (oldFreq == minFreq_)  // 如果旧频率是minFreq_，将minFreq_更新为新频率
            {
                minFreq_ = newFreq;
            }
        }

        // 添加到新频率列表
        if (freqMap_.find(newFreq) == freqMap_.end()) // 如果新频率的列表不存在，创建新的列表
        {
            freqMap_[newFreq] = std::list<NodePtr>();
        }
        freqMap_[newFreq].push_back(node); // 将节点添加到列表末尾
    }

    void evictLeastFrequent() // 淘汰最不常用的节点
    {
        if (freqMap_.empty()) 
            return;

        // 获取最小频率的列表
        auto& minFreqList = freqMap_[minFreq_];
        if (minFreqList.empty()) 
            return;

        // 移除最少使用的节点
        NodePtr leastNode = minFreqList.front(); // 取出列表中的第一个节点（最久未访问的）   为什么这里可以使用front？ 因为FreqMap_是STL中的list，因此可以使用front，自定义的链表结构则不可以使用front
        minFreqList.pop_front(); // 从列表中移除

        // 如果该频率的列表为空，则删除该频率项
        if (minFreqList.empty()) 
        {
            freqMap_.erase(minFreq_);
            // 更新最小频率
            if (!freqMap_.empty()) 
            {
                minFreq_ = freqMap_.begin()->first; // 更新minFreq_为freqMap_中的第一个键（最小频率）
            }
        }

        // 将淘汰节点移到幽灵缓存
        if (ghostCache_.size() >= ghostCapacity_) // 如果幽灵缓存已满，移除最旧的幽灵记录
        {
            removeOldestGhost();
        }
        addToGhost(leastNode); // 将节点添加到幽灵缓存
        
        // 从主缓存中移除
        mainCache_.erase(leastNode->getKey()); // 从主缓存映射中移除
    }

    void removeFromGhost(NodePtr node) // 从幽灵链表中移除节点
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // 清空指针，防止悬垂引用
        }
    }

    void addToGhost(NodePtr node) // 将节点插入到幽灵链表尾部（ghostTail_之前）     与LRU部分添加到头部不同，这里添加到尾部
    {
        node->next_ = ghostTail_;
        node->prev_ = ghostTail_->prev_;
        if (!ghostTail_->prev_.expired()) { // 检查前驱节点是否有效
            ghostTail_->prev_.lock()->next_ = node;
        }
        ghostTail_->prev_ = node;
        ghostCache_[node->getKey()] = node; // 添加到幽灵缓存映射
    }

    void removeOldestGhost() // 移除最旧的幽灵记录
    {
        NodePtr oldestGhost = ghostHead_->next_; // 获取幽灵链表头部的下一个节点（最旧的）
        if (oldestGhost != ghostTail_) // 如果不是尾节点，从链表中移除并从映射中擦除
        {
            removeFromGhost(oldestGhost);
            ghostCache_.erase(oldestGhost->getKey());
        }
    }

private:
    size_t capacity_; // LFU主缓存容量
    size_t ghostCapacity_; // 幽灵缓存容量
    size_t transformThreshold_; // 转换阈值
    size_t minFreq_; // 当前最小频率
    std::mutex mutex_;

    NodeMap mainCache_; // 主缓存哈希映射
    NodeMap ghostCache_; // 幽灵缓存哈希映射
    FreqMap freqMap_; // 频率到节点列表的映射（std::map保证有序）
    
    NodePtr ghostHead_;
    NodePtr ghostTail_;
};

} // namespace KamaCache