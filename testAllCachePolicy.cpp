#include <iostream>
#include <string>
#include <chrono> // 包含时间库，用于性能计时
#include <vector>
#include <iomanip> // 包含输入输出格式化，用于std::fixed和std::setprecision
#include <random> // 包含随机数生成库
#include <algorithm>
#include<array>
#include "KICachePolicy.h"
#include "KLfuCache.h"
#include "KLruCache.h"
#include "KArcCache/KArcCache.h"

class Timer {
public:
    Timer() : start_(std::chrono::high_resolution_clock::now()) {} // 计时器类，构造函数记录开始时间   high_resolution_clock：提供最高精度的系统时钟
    
    double elapsed() { // elapsed()方法：返回从创建Timer到调用时经过的毫秒数
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now - start_).count(); // duration_cast    作用：将时间段从一个单位转换为另一个单位
    }

private:
    std::chrono::time_point<std::chrono::high_resolution_clock> start_; // 存储开始时间点的成员变量
}; // time_point：表示一个具体的时间点

// 辅助函数：打印结果
void printResults(const std::string& testName, int capacity, // printResults函数：打印测试结果
                 const std::vector<int>& get_operations, // get操作次数
                 const std::vector<int>& hits) { // 各种算法对应的命中次数
    std::cout << "=== " << testName << " 结果汇总 ===" << std::endl;
    std::cout << "缓存大小: " << capacity << std::endl;
    
    // 假设对应的算法名称已在测试函数中定义
    std::vector<std::string> names;
    if (hits.size() == 3) {
        names = {"LRU", "LFU", "ARC"};
    } else if (hits.size() == 4) {
        names = {"LRU", "LFU", "ARC", "LRU-K"};
    } else if (hits.size() == 5) {
        names = {"LRU", "LFU", "ARC", "LRU-K", "LFU-Aging"};
    }
    
    for (size_t i = 0; i < hits.size(); ++i) { // 遍历所有算法，计算并打印命中率
        double hitRate = 100.0 * hits[i] / get_operations[i]; // 显示百分比（保留两位小数）和具体命中次数/总操作次数
        std::cout << (i < names.size() ? names[i] : "Algorithm " + std::to_string(i+1)) 
                  << " - 命中率: " << std::fixed << std::setprecision(2) // std::fixed  设置浮点数输出格式为固定小数点表示法，否则有可能输出科学计数法
                  << hitRate << "% ";
        // 添加具体命中次数和总操作次数
        std::cout << "(" << hits[i] << "/" << get_operations[i] << ")" << std::endl;
    }
    
    std::cout << std::endl;  // 添加空行，使输出更清晰
}

void testHotDataAccess() {
    std::cout << "\n=== 测试场景1：热点数据访问测试 ===" << std::endl;
    
    const int CAPACITY = 20;         // 缓存容量
    const int OPERATIONS = 500000;   // 总操作次数
    const int HOT_KEYS = 20;         // 热点数据数量
    const int COLD_KEYS = 5000;      // 冷数据数量
    // 创建三种缓存实例
    KamaCache::KLruCache<int, std::string> lru(CAPACITY);
    KamaCache::KLfuCache<int, std::string> lfu(CAPACITY);
    KamaCache::KArcCache<int, std::string> arc(CAPACITY);
    // 为LRU-K设置合适的参数：
    // - 主缓存容量与其他算法相同
    // - 历史记录容量设为可能访问的所有键数量
    // - k=2表示数据被访问2次后才会进入缓存，适合区分热点和冷数据
    KamaCache::KLruKCache<int, std::string> lruk(CAPACITY, HOT_KEYS + COLD_KEYS, 2); // 创建LRU-K缓存实例
    KamaCache::KLfuCache<int, std::string> lfuAging(CAPACITY, 20000); // 创建带老化机制的LFU缓存实例    频率衰减阈值设置为20000
    // 创建随机数生成器
    std::random_device rd; // 用于获取真随机种子
    std::mt19937 gen(rd()); // 梅森旋转算法伪随机数生成器
    
    // 基类指针指向派生类对象，添加LFU-Aging
    std::array<KamaCache::KICachePolicy<int, std::string>*, 5> caches = {&lru, &lfu, &arc, &lruk, &lfuAging};
    std::vector<int> hits(5, 0);
    std::vector<int> get_operations(5, 0);
    std::vector<std::string> names = {"LRU", "LFU", "ARC", "LRU-K", "LFU-Aging"};

    // 为所有的缓存对象进行相同的操作序列测试
    for (int i = 0; i < caches.size(); ++i) {
        // 先预热缓存，插入一些数据
        for (int key = 0; key < HOT_KEYS; ++key) {
            std::string value = "value" + std::to_string(key);
            caches[i]->put(key, value);
        }
        
        // 交替进行put和get操作，模拟真实场景
        for (int op = 0; op < OPERATIONS; ++op) {
            // 大多数缓存系统中读操作比写操作频繁
            // 所以设置30%概率进行写操作
            bool isPut = (gen() % 100 < 30); 
            int key;
            
            // 70%概率访问热点数据，30%概率访问冷数据
            if (gen() % 100 < 70) {
                key = gen() % HOT_KEYS; // 热点数据
            } else {
                key = HOT_KEYS + (gen() % COLD_KEYS); // 冷数据
            }
            
            if (isPut) {
                // 执行put操作
                std::string value = "value" + std::to_string(key) + "_v" + std::to_string(op % 100);
                caches[i]->put(key, value);
            } else {
                // 执行get操作并记录命中情况
                std::string result;
                get_operations[i]++;
                if (caches[i]->get(key, result)) {
                    hits[i]++;
                }
            }
        }
    }

    // 打印测试结果
    printResults("热点数据访问测试", CAPACITY, get_operations, hits);
}

void testLoopPattern() {
    std::cout << "\n=== 测试场景2：循环扫描测试 ===" << std::endl;
    
    const int CAPACITY = 50;          // 缓存容量
    const int LOOP_SIZE = 500;        // 循环范围大小
    const int OPERATIONS = 200000;    // 总操作次数
    
    KamaCache::KLruCache<int, std::string> lru(CAPACITY);
    KamaCache::KLfuCache<int, std::string> lfu(CAPACITY);
    KamaCache::KArcCache<int, std::string> arc(CAPACITY);
    // 为LRU-K设置合适的参数：
    // - 历史记录容量设为总循环大小的两倍，覆盖范围内和范围外的数据
    // - k=2，对于循环访问，这是一个合理的阈值
    KamaCache::KLruKCache<int, std::string> lruk(CAPACITY, LOOP_SIZE * 2, 2);
    KamaCache::KLfuCache<int, std::string> lfuAging(CAPACITY, 3000);

    std::array<KamaCache::KICachePolicy<int, std::string>*, 5> caches = {&lru, &lfu, &arc, &lruk, &lfuAging};
    std::vector<int> hits(5, 0);
    std::vector<int> get_operations(5, 0);
    std::vector<std::string> names = {"LRU", "LFU", "ARC", "LRU-K", "LFU-Aging"};

    std::random_device rd;
    std::mt19937 gen(rd());

    // 为每种缓存算法运行相同的测试
    for (int i = 0; i < caches.size(); ++i) {
        // 先预热一部分数据（只加载20%的数据）
        for (int key = 0; key < LOOP_SIZE / 5; ++key) {
            std::string value = "loop" + std::to_string(key);
            caches[i]->put(key, value);
        }
        
        // 设置循环扫描的当前位置
        int current_pos = 0;
        
        // 交替进行读写操作，模拟真实场景
        for (int op = 0; op < OPERATIONS; ++op) {
            // 20%概率是写操作，80%概率是读操作
            bool isPut = (gen() % 100 < 20);
            int key;
            
            // 按照不同模式选择键
            if (op % 100 < 60) {  // 60%顺序扫描
                key = current_pos;
                current_pos = (current_pos + 1) % LOOP_SIZE;
            } else if (op % 100 < 90) {  // 30%随机跳跃
                key = gen() % LOOP_SIZE;
            } else {  // 10%访问范围外数据
                key = LOOP_SIZE + (gen() % LOOP_SIZE);
            }
            
            if (isPut) {
                // 执行put操作，更新数据
                std::string value = "loop" + std::to_string(key) + "_v" + std::to_string(op % 100);
                caches[i]->put(key, value);
            } else {
                // 执行get操作并记录命中情况
                std::string result;
                get_operations[i]++;
                if (caches[i]->get(key, result)) {
                    hits[i]++;
                }
            }
        }
    }

    printResults("循环扫描测试", CAPACITY, get_operations, hits);
}

void testWorkloadShift() {
    std::cout << "\n=== 测试场景3：工作负载剧烈变化测试 ===" << std::endl;
    
    const int CAPACITY = 30;            // 缓存容量
    const int OPERATIONS = 80000;       // 总操作次数
    const int PHASE_LENGTH = OPERATIONS / 5;  // 每个阶段的长度    每阶段16000次操作（共5个阶段）
    
    KamaCache::KLruCache<int, std::string> lru(CAPACITY);
    KamaCache::KLfuCache<int, std::string> lfu(CAPACITY);
    KamaCache::KArcCache<int, std::string> arc(CAPACITY);
    KamaCache::KLruKCache<int, std::string> lruk(CAPACITY, 500, 2);
    KamaCache::KLfuCache<int, std::string> lfuAging(CAPACITY, 10000);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::array<KamaCache::KICachePolicy<int, std::string>*, 5> caches = {&lru, &lfu, &arc, &lruk, &lfuAging};
    std::vector<int> hits(5, 0);
    std::vector<int> get_operations(5, 0);
    std::vector<std::string> names = {"LRU", "LFU", "ARC", "LRU-K", "LFU-Aging"};

    // 为每种缓存算法运行相同的测试
    for (int i = 0; i < caches.size(); ++i) { 
        // 先预热缓存，只插入少量初始数据
        for (int key = 0; key < 30; ++key) {
            std::string value = "init" + std::to_string(key);
            caches[i]->put(key, value);
        }
        
        // 进行多阶段测试，每个阶段有不同的访问模式
        for (int op = 0; op < OPERATIONS; ++op) {
            // 确定当前阶段
            int phase = op / PHASE_LENGTH;
            
            // 每个阶段的读写比例不同 
            int putProbability;
            switch (phase) {
                case 0: putProbability = 15; break;  // 阶段1: 热点访问，15%写入更合理
                case 1: putProbability = 30; break;  // 阶段2: 大范围随机，写比例为30%
                case 2: putProbability = 10; break;  // 阶段3: 顺序扫描，10%写入保持不变
                case 3: putProbability = 25; break;  // 阶段4: 局部性随机，微调为25%
                case 4: putProbability = 20; break;  // 阶段5: 混合访问，调整为20%
                default: putProbability = 20;
            }
            
            // 确定是读还是写操作
            bool isPut = (gen() % 100 < putProbability);
            
            // 根据不同阶段选择不同的访问模式生成key - 优化后的访问范围
            int key;
            if (op < PHASE_LENGTH) {  // 阶段1: 热点访问 - 热点数量5，使热点更集中
                key = gen() % 5;
            } else if (op < PHASE_LENGTH * 2) {  // 阶段2: 大范围随机 - 范围400，更适合30大小的缓存
                key = gen() % 400;
            } else if (op < PHASE_LENGTH * 3) {  // 阶段3: 顺序扫描 - 保持100个键
                key = (op - PHASE_LENGTH * 2) % 100;
            } else if (op < PHASE_LENGTH * 4) {  // 阶段4: 局部性随机 - 优化局部性区域大小
                // 产生5个局部区域，每个区域大小为15个键，与缓存大小20接近但略小
                int locality = (op / 800) % 5;  // 调整为5个局部区域
                key = locality * 15 + (gen() % 15);  // 每区域15个键
            } else {  // 阶段5: 混合访问 - 增加热点访问比例
                int r = gen() % 100;
                if (r < 40) {  // 40%概率访问热点（从30%增加）
                    key = gen() % 5;  // 5个热点键
                } else if (r < 70) {  // 30%概率访问中等范围
                    key = 5 + (gen() % 45);  // 缩小中等范围为50个键
                } else {  // 30%概率访问大范围（从40%减少）
                    key = 50 + (gen() % 350);  // 大范围也相应缩小
                }
            }
            
            if (isPut) {
                // 执行写操作
                std::string value = "value" + std::to_string(key) + "_p" + std::to_string(phase);
                caches[i]->put(key, value);
            } else {
                // 执行读操作并记录命中情况
                std::string result;
                get_operations[i]++;
                if (caches[i]->get(key, result)) {
                    hits[i]++;
                }
            }
        }
    }

    printResults("工作负载剧烈变化测试", CAPACITY, get_operations, hits);
}

int main() { // 依次运行三个测试场景
    testHotDataAccess(); // 1.热点数据访问测试
    testLoopPattern(); // 2.循环扫描测试
    testWorkloadShift(); // 3.工作负载剧烈变化测试
    return 0;
}