#pragma once

#include "KArcCacheNode.h"
#include <unordered_map>
#include <mutex>

namespace KamaCache 
{

template<typename Key, typename Value>
class ArcLruPart // 定义ARC算法中的LRU部分模板类       ARC（Adaptive Replacement Cache）算法包含LRU和LFU两部分，这是其中的LRU部分
{
public:
    using NodeType = ArcNode<Key, Value>; // ARC节点类型
    using NodePtr = std::shared_ptr<NodeType>; // 节点的共享指针
    using NodeMap = std::unordered_map<Key, NodePtr>; // 从key到节点的哈希映射

    explicit ArcLruPart(size_t capacity, size_t transformThreshold) // 构造函数
        : capacity_(capacity) // LRU主缓存容量
        , ghostCapacity_(capacity) // 幽灵缓存容量，与主缓存容量相同
        , transformThreshold_(transformThreshold) // 转换阈值，当节点访问次数达到此值时，将从LRU部分转移到LFU部分
    {
        initializeLists(); // 调用initializeLists()初始化链表
    }

    bool put(Key key, Value value) 
    {
        if (capacity_ == 0) return false;
        
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) 
        {
            return updateExistingNode(it->second, value); // 如果key已存在：更新值并移到链表头部
        }
        return addNewNode(key, value); // 如果key不存在：添加新节点
    }

    bool get(Key key, Value& value, bool& shouldTransform) 
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = mainCache_.find(key);
        if (it != mainCache_.end()) //如果找到：更新节点访问，通过shouldTransform指示是否需要转移到LFU部分，通过引用返回value
        {
            shouldTransform = updateNodeAccess(it->second);
            value = it->second->getValue();
            return true;
        }
        return false; // 如果没找到：返回false
    }

    bool checkGhost(Key key) // 检查幽灵缓存      幽灵缓存记录了最近被淘汰的key，用于ARC算法的自适应调整
    {
        auto it = ghostCache_.find(key);
        if (it != ghostCache_.end()) { // 如果key在幽灵缓存中，移除并返回true
            removeFromGhost(it->second);
            ghostCache_.erase(it);
            return true;
        }
        return false;
    }

    void increaseCapacity() { ++capacity_; } // 增加LRU部分的容量（ARC算法会根据命中率动态调整LRU和LFU的容量比例）
    
    bool decreaseCapacity() // 减少LRU部分的容量
    {
        if (capacity_ <= 0) return false;
        if (mainCache_.size() == capacity_) { // 如果容量已满，先淘汰一个元素
            evictLeastRecent();
        }
        --capacity_; // 容量减1
        return true;
    }

private:
    void initializeLists() // 初始化双向链表
    {
        mainHead_ = std::make_shared<NodeType>();
        mainTail_ = std::make_shared<NodeType>();
        mainHead_->next_ = mainTail_;
        mainTail_->prev_ = mainHead_;

        ghostHead_ = std::make_shared<NodeType>();
        ghostTail_ = std::make_shared<NodeType>();
        ghostHead_->next_ = ghostTail_;
        ghostTail_->prev_ = ghostHead_;
    }

    bool updateExistingNode(NodePtr node, const Value& value) // 更新已有节点：设置新值并移到链表头部
    {
        node->setValue(value);
        moveToFront(node);
        return true;
    }

    bool addNewNode(const Key& key, const Value& value) // 添加新节点
    {
        if (mainCache_.size() >= capacity_) // 如果缓存已满
        {   
            evictLeastRecent(); // 驱逐最近最少访问的节点
        }

        NodePtr newNode = std::make_shared<NodeType>(key, value); // 创建新节点，加入哈希映射，添加到链表头部
        mainCache_[key] = newNode;
        addToFront(newNode);
        return true;
    }

    bool updateNodeAccess(NodePtr node) // 更新节点访问
    {
        moveToFront(node); // 移到链表头部
        node->incrementAccessCount(); // 访问次数加1
        return node->getAccessCount() >= transformThreshold_; // 返回是否达到转换阈值（需要转移到LFU部分）
    }

    void moveToFront(NodePtr node) // 将节点移到链表头部
    {
        // 先从当前位置移除
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // 清空指针，防止悬垂引用
        }
        
        // 添加到头部
        addToFront(node);
    }

    void addToFront(NodePtr node) // 将节点添加到链表头部（最近使用位置）
    {
        node->next_ = mainHead_->next_;
        node->prev_ = mainHead_;
        mainHead_->next_->prev_ = node;
        mainHead_->next_ = node;
    }

    void evictLeastRecent() // 驱逐最近最少使用的节点
    {
        NodePtr leastRecent = mainTail_->prev_.lock(); // 获取尾节点的前一个节点（最久未使用的节点）
        if (!leastRecent || leastRecent == mainHead_) 
            return;

        // 从主链表中移除
        removeFromMain(leastRecent);

        // 添加到幽灵缓存
        if (ghostCache_.size() >= ghostCapacity_) // 如果幽灵缓存已满，移除最旧的幽灵记录
        {
            removeOldestGhost();
        }
        addToGhost(leastRecent); // 将节点添加到幽灵缓存（记录被淘汰的key）

        // 从主缓存映射中移除
        mainCache_.erase(leastRecent->getKey());
    }

    void removeFromMain(NodePtr node) // 从主链表中移除节点
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // 清空指针，防止悬垂引用
        }
    }

    void removeFromGhost(NodePtr node) // 从幽灵链表中移除节点，逻辑与removeFromMain相同
    {
        if (!node->prev_.expired() && node->next_) {
            auto prev = node->prev_.lock();
            prev->next_ = node->next_;
            node->next_->prev_ = node->prev_;
            node->next_ = nullptr; // 清空指针，防止悬垂引用
        }
    }

    void addToGhost(NodePtr node) // 将节点添加到幽灵缓存
    {
        // 重置节点的访问计数
        node->accessCount_ = 1;
        
        // 添加到幽灵缓存的头部
        node->next_ = ghostHead_->next_;
        node->prev_ = ghostHead_;
        ghostHead_->next_->prev_ = node;
        ghostHead_->next_ = node;
        
        // 添加到幽灵缓存映射
        ghostCache_[node->getKey()] = node;
    }

    void removeOldestGhost() // 移除最旧的幽灵记录
    {
        // 使用lock()方法，并添加null检查
        NodePtr oldestGhost = ghostTail_->prev_.lock(); // 获取幽灵链表尾节点的前一个节点
        if (!oldestGhost || oldestGhost == ghostHead_) // 如果为空或是头节点，直接返回
            return;

        removeFromGhost(oldestGhost); // 从幽灵链表中移除
        ghostCache_.erase(oldestGhost->getKey()); // 从幽灵缓存映射中擦除
    }
    

private:
    size_t capacity_; // LRU主缓存容量
    size_t ghostCapacity_; // 幽灵缓存容量
    size_t transformThreshold_; // 转换门槛值
    std::mutex mutex_;

    NodeMap mainCache_; // key -> ArcNode   主缓存哈希映射
    NodeMap ghostCache_; // 幽灵缓存哈希映射
    
    // 主链表
    NodePtr mainHead_;
    NodePtr mainTail_;
    // 淘汰链表     幽灵链表虚拟头尾节点
    NodePtr ghostHead_;
    NodePtr ghostTail_;
};

} // namespace KamaCache