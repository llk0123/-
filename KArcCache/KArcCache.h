#pragma once

#include "../KICachePolicy.h" // 包含缓存策略接口，KArcCache继承自此接口
#include "KArcLruPart.h" // 包含ARC算法的LRU部分实现
#include "KArcLfuPart.h" // 包含ARC算法的LFU部分实现
#include <memory>

namespace KamaCache // LRU负责"新鲜度"，LRU记录最近访问的数据      LFU负责"频率"，LFU记录长期的热点数据
{

template<typename Key, typename Value>
class KArcCache : public KICachePolicy<Key, Value> // 定义ARC缓存主类，继承自缓存策略接口   ARC是一种自适应的缓存替换算法，结合了LRU和LFU的优点
{
public:
    explicit KArcCache(size_t capacity = 10, size_t transformThreshold = 2) // 构造函数
        : capacity_(capacity) // capacity: 缓存总容量，默认10
        , transformThreshold_(transformThreshold) // 转换阈值，默认2（当LRU中某节点访问次数达到此值，转移到LFU）
        , lruPart_(std::make_unique<ArcLruPart<Key, Value>>(capacity, transformThreshold)) // 使用std::make_unique创建LRU和LFU两个部分的实例
        , lfuPart_(std::make_unique<ArcLfuPart<Key, Value>>(capacity, transformThreshold)) // 两个部分初始容量相同，各占一半
    {}

    ~KArcCache() override = default; // 默认析构函数，unique_ptr会自动释放LRU和LFU部分

    void put(Key key, Value value) override
    {
        checkGhostCaches(key); // 首先检查幽灵缓存，根据命中情况动态调整LRU和LFU的容量比例

        // 先检查 key 是否在LFU部分
        bool inLfu = lfuPart_->contain(key);
        // 无论如何都先更新LRU部分（新数据先进入LRU）
        lruPart_->put(key, value);
        // 如果 key 在LFU部分也存在，同时更新LFU部分   为什么key会同时存在于LRU和LFU？    数据可以从LRU部分晋升到LFU部分，但晋升后并不会从LRU部分删除
        if (inLfu) 
        {
            lfuPart_->put(key, value);
        }
    }

    bool get(Key key, Value& value) override 
    {
        checkGhostCaches(key); // 先检查幽灵缓存，调整容量比例

        bool shouldTransform = false;
        if (lruPart_->get(key, value, shouldTransform)) // 先在LRU部分查找
        {
            if (shouldTransform) // 如果找到，shouldTransform指示是否需要转移到LFU
            {
                lfuPart_->put(key, value); // 如果需要转移，将数据放入LFU部分
            }
            return true;
        }
        return lfuPart_->get(key, value); // 如果LRU部分未命中，再到LFU部分查找，返回LFU部分的查找结果
    }

    Value get(Key key) override // 按值返回的get方法，存在无法区分"未命中"和"命中但值为默认值"的问题
    {
        Value value{};
        get(key, value);
        return value;
    }

private:
    bool checkGhostCaches(Key key) // 检查幽灵缓存的核心逻辑
    {
        bool inGhost = false;
        if (lruPart_->checkGhost(key)) // 如果在LRU的幽灵缓存中命中，说明这个key曾被LRU淘汰，但现在又被访问。应该增加LRU的容量，减少LFU的容量（因为LRU更"需要"这个key）
        {
            if (lfuPart_->decreaseCapacity()) // lfuPart_->decreaseCapacity()返回true表示成功减少
            {
                lruPart_->increaseCapacity();
            }
            inGhost = true;
        } 
        else if (lfuPart_->checkGhost(key)) // 如果在LFU的幽灵缓存中命中，说明这个key曾被LFU淘汰，但现在又被访问，应该增加LFU的容量，减少LRU的容量
        {
            if (lruPart_->decreaseCapacity()) 
            {
                lfuPart_->increaseCapacity();
            }
            inGhost = true;
        }
        return inGhost; // 返回是否在幽灵缓存中命中
    }

private:
    size_t capacity_; // 缓存总容量
    size_t transformThreshold_; // 转换阈值
    std::unique_ptr<ArcLruPart<Key, Value>> lruPart_; // LRU部分的智能指针
    std::unique_ptr<ArcLfuPart<Key, Value>> lfuPart_; // LFU部分的智能指针
};

} // namespace KamaCache