#include "WindowsDirectShowSink.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>

using namespace EyeCam;

// Генератор тестового кадру в форматі YUY2 (YUYV) з анімованими колірними смугами та лічильником
void GenerateYUY2TestPattern(std::vector<uint8_t>& buffer, uint32_t width, uint32_t height, uint32_t frameNum) {
    size_t expectedSize = CalculateFrameSize(width, height, PixelFormat::YUY2);
    if (buffer.size() != expectedSize) {
        buffer.resize(expectedSize);
    }

    int boxSize = 120;
    int boxX = static_cast<int>((frameNum * 10) % (width - boxSize));
    int boxY = static_cast<int>((height / 2) + std::sin(frameNum * 0.08) * (height / 3.5));

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; x += 2) {
            size_t idx = (y * width + x) * 2;

            bool inBox0 = (static_cast<int>(x) >= boxX && static_cast<int>(x) < boxX + boxSize &&
                           static_cast<int>(y) >= boxY && static_cast<int>(y) < boxY + boxSize);
            bool inBox1 = (static_cast<int>(x + 1) >= boxX && static_cast<int>(x + 1) < boxX + boxSize &&
                           static_cast<int>(y) >= boxY && static_cast<int>(y) < boxY + boxSize);

            // Плавний горизонтальний градієнт
            uint8_t luma0 = static_cast<uint8_t>((x * 200) / width + 20);
            uint8_t luma1 = static_cast<uint8_t>(((x + 1) * 200) / width + 20);

            if (inBox0 || inBox1) {
                buffer[idx]     = inBox0 ? 230 : luma0; // Y0
                buffer[idx + 1] = 60;                   // U
                buffer[idx + 2] = inBox1 ? 230 : luma1; // Y1
                buffer[idx + 3] = 220;                  // V
            } else {
                buffer[idx]     = luma0;
                buffer[idx + 1] = 128;
                buffer[idx + 2] = luma1;
                buffer[idx + 3] = 128;
            }
        }
    }
}

int main() {
    std::cout << "====================================================\n";
    std::cout << "  EyeCam Windows DirectShow Virtual Camera Test\n";
    std::cout << "  Продюсер потоку для Zoom, Teams, Discord, OBS\n";
    std::cout << "====================================================\n";

    WindowsDirectShowSink sink;

    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;

    std::cout << "[1] Ініціалізація спільної пам'яті: " << width << "x" << height 
              << " @ " << fps << " FPS (YUY2)...\n";

    if (!sink.Open(width, height, PixelFormat::YUY2, fps)) {
        std::cerr << "[-] Помилка ініціалізації: " << sink.GetLastError() << "\n";
        return 1;
    }

    std::cout << "[+] Канал спільної пам'яті відкрито!\n";
    std::cout << "[*] Запустіть Zoom / OBS / Discord / браузер та виберіть 'EyeCam Virtual Camera'.\n";

    std::vector<uint8_t> frameBuffer;
    auto frameInterval = std::chrono::nanoseconds(1000000000ULL / fps);

    // 2. Стрімінг 1080p @ 60 FPS (300 кадрів, ~5 секунд)
    std::cout << "\n[2] Початок стрімінгу 1080p @ 60 FPS (300 кадрів)...\n";
    for (uint32_t i = 0; i < 300; ++i) {
        auto start = std::chrono::steady_clock::now();

        GenerateYUY2TestPattern(frameBuffer, width, height, i);
        if (!sink.WriteFrame(frameBuffer.data(), frameBuffer.size())) {
            std::cerr << "[-] Помилка запису: " << sink.GetLastError() << "\n";
            break;
        }

        if (i % 60 == 0) {
            std::cout << "    [1080p 60fps] Кадр #" << i << " записано в буфер\n";
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed < frameInterval) {
            std::this_thread::sleep_for(frameInterval - elapsed);
        }
    }

    // 3. Динамічна зміна роздільної здатності на льоту (Dynamic Resizing)
    width = 1280;
    height = 720;
    fps = 30;
    frameInterval = std::chrono::nanoseconds(1000000000ULL / fps);

    std::cout << "\n[3] Тест динамічного масштабування -> " << width << "x" << height 
              << " @ " << fps << " FPS...\n";

    if (!sink.SetFormat(width, height, PixelFormat::YUY2, fps)) {
        std::cerr << "[-] Помилка динамічної зміни роздільної здатності: " << sink.GetLastError() << "\n";
    } else {
        std::cout << "[+] Роздільну здатність успішно змінено на льоту!\n";

        for (uint32_t i = 0; i < 150; ++i) {
            auto start = std::chrono::steady_clock::now();

            GenerateYUY2TestPattern(frameBuffer, width, height, i);
            if (!sink.WriteFrame(frameBuffer.data(), frameBuffer.size())) {
                std::cerr << "[-] Помилка запису: " << sink.GetLastError() << "\n";
                break;
            }

            if (i % 30 == 0) {
                std::cout << "    [720p 30fps] Кадр #" << i << " записано в буфер\n";
            }

            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed < frameInterval) {
                std::this_thread::sleep_for(frameInterval - elapsed);
            }
        }
    }

    sink.Close();
    std::cout << "\n[+] Тест успішно завершено. Спільну пам'ять закрито.\n";
    return 0;
}
