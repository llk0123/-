#pragma once

#include <memory> // 包含智能指针头文件

namespace KamaCache 
{

template<typename Key, typename Value>
class ArcNode // 定义一个模板类ArcNode，用于ARC（Adaptive Replacement Cache）缓存算法的节点
{
private:
    Key key_;
    Value value_;
    size_t accessCount_; // accessCount_: 记录节点的访问次数，用于LFU部分的频率统计
    std::weak_ptr<ArcNode> prev_; // prev_: 指向前一个节点的弱指针，使用weak_ptr打破循环引用，避免内存泄漏
    std::shared_ptr<ArcNode> next_; // next_: 指向后一个节点的共享指针

public:
    ArcNode() : accessCount_(1), next_(nullptr) {} // 构造函数  初始化accessCount_为1（表示创建即算一次访问）  初始化next_为空指针  key_/value_/prev_采用默认构造
    
    ArcNode(Key key, Value value) // 带参数的构造函数   剩余成员使用默认初始化
        : key_(key)
        , value_(value)
        , accessCount_(1)
        , next_(nullptr) 
    {}

    // Getters
    Key getKey() const { return key_; }
    Value getValue() const { return value_; }
    size_t getAccessCount() const { return accessCount_; }
    
    // Setters
    void setValue(const Value& value) { value_ = value; } // 设置节点的新值，接收常量引用以避免拷贝
    void incrementAccessCount() { ++accessCount_; } // 将访问次数加1
    // 使用模板友元语法，确保不同模板参数的实例化也能访问
    template<typename K, typename V> friend class ArcLruPart; // 将ArcLruPart和ArcLfuPart声明为友元类
    template<typename K, typename V> friend class ArcLfuPart; // 这两个类可以访问ArcNode的私有成员
};

} // namespace KamaCache