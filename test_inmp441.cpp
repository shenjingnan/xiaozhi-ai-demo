#include <iostream>
#include <vector>
#include <cmath>

// 模拟I2S接口
class I2SInterface {
public:
    bool init() {
        std::cout << "初始化I2S接口" << std::endl;
        return true;
    }

    bool read(std::vector<int32_t>& data, int samples) {
        data.resize(samples);
        // 生成一些模拟数据
        for (int i = 0; i < samples; i++) {
            data[i] = static_cast<int32_t>(sin(i * 0.1) * 1000000);  // 模拟32位数据
        }
        return true;
    }
};

// 模拟INMP441麦克风
class INMP441 {
private:
    I2SInterface i2s;
    const int sample_rate;
    const int bits_per_sample;

public:
    INMP441(int rate = 16000, int bits = 32) 
        : sample_rate(rate), bits_per_sample(bits) {}

    bool init() {
        std::cout << "初始化INMP441麦克风" << std::endl;
        return i2s.init();
    }

    bool readFrame(std::vector<int16_t>& pcm_data, int frame_size) {
        std::vector<int32_t> raw_data;
        if (!i2s.read(raw_data, frame_size)) {
            return false;
        }

        // 将32位数据转换为16位PCM
        pcm_data.resize(frame_size);
        for (int i = 0; i < frame_size; i++) {
            // 模拟INMP441的左对齐32位数据，右移16位得到16位PCM
            pcm_data[i] = static_cast<int16_t>(raw_data[i] >> 16);
        }
        return true;
    }
};

// 模拟音频处理
class AudioProcessor {
private:
    INMP441 mic;
    const int frame_size;

public:
    AudioProcessor(int size = 320) : frame_size(size) {}

    bool init() {
        std::cout << "初始化音频处理器" << std::endl;
        return mic.init();
    }

    void process() {
        std::cout << "开始处理音频..." << std::endl;
        
        std::vector<int16_t> pcm_data;
        
        // 模拟处理10帧
        for (int i = 0; i < 10; i++) {
            if (mic.readFrame(pcm_data, frame_size)) {
                // 计算音频能量
                double energy = 0;
                for (int j = 0; j < pcm_data.size(); j++) {
                    energy += pcm_data[j] * pcm_data[j];
                }
                energy /= pcm_data.size();
                
                std::cout << "帧 " << i << ": 采样点数=" << pcm_data.size() 
                          << ", 能量=" << energy 
                          << ", 第一个采样值=" << pcm_data[0] << std::endl;
            }
        }
    }
};

int main() {
    std::cout << "INMP441麦克风测试程序" << std::endl;
    
    AudioProcessor processor;
    if (processor.init()) {
        processor.process();
    } else {
        std::cout << "初始化失败" << std::endl;
        return 1;
    }
    
    std::cout << "测试完成" << std::endl;
    return 0;
}
