#include "LinuxV4L2Sink.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>

using namespace EyeCam;

// Генератор тестового кадру в форматі YUY2 (YUYV) з анімованим рухомим прямокутником
void GenerateYUY2Frame(std::vector<uint8_t>& buffer, uint32_t width, uint32_t height, uint32_t frameNum) {
    size_t expectedSize = CalculateFrameSize(width, height, PixelFormat::YUY2);
    if (buffer.size() != expectedSize) {
        buffer.resize(expectedSize);
    }

    int boxSize = 100;
    int boxX = static_cast<int>((frameNum * 8) % (width - boxSize));
    int boxY = static_cast<int>((height / 2) + std::sin(frameNum * 0.05) * (height / 4));

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; x += 2) {
            size_t idx = (y * width + x) * 2;

            bool inBox0 = (static_cast<int>(x) >= boxX && static_cast<int>(x) < boxX + boxSize &&
                           static_cast<int>(y) >= boxY && static_cast<int>(y) < boxY + boxSize);
            bool inBox1 = (static_cast<int>(x + 1) >= boxX && static_cast<int>(x + 1) < boxX + boxSize &&
                           static_cast<int>(y) >= boxY && static_cast<int>(y) < boxY + boxSize);

            // Фон: горизонтальний градієнт яскравості
            uint8_t bgY0 = static_cast<uint8_t>((x * 210) / width + 16);
            uint8_t bgY1 = static_cast<uint8_t>(((x + 1) * 210) / width + 16);
            uint8_t bgU = 128;
            uint8_t bgV = 128;

            // Анімований об'єкт: яскравий колір
            if (inBox0 || inBox1) {
                buffer[idx]     = inBox0 ? 235 : bgY0; // Y0
                buffer[idx + 1] = 240;                 // U (Chrominance)
                buffer[idx + 2] = inBox1 ? 235 : bgY1; // Y1
                buffer[idx + 3] = 110;                 // V (Chrominance)
            } else {
                buffer[idx]     = bgY0;
                buffer[idx + 1] = bgU;
                buffer[idx + 2] = bgY1;
                buffer[idx + 3] = bgV;
            }
        }
    }
}

int main(int argc, char* argv[]) {
    std::string devicePath = "/dev/video0";
    if (argc > 1) {
        devicePath = argv[1];
    }

    std::cout << "====================================================\n";
    std::cout << "  EyeCam Linux V4L2 Virtual Camera Sink Test\n";
    std::cout << "  Пристрій: " << devicePath << "\n";
    std::cout << "====================================================\n";

    LinuxV4L2Sink sink;

    // 1. Початкова конфігурація: 1920x1080 @ 60 FPS, YUY2
    uint32_t width = 1920;
    uint32_t height = 1080;
    uint32_t fps = 60;

    std::cout << "[1] Відкриття пристрою: " << width << "x" << height 
              << " @ " << fps << " FPS (YUY2)...\n";
    if (!sink.Open(devicePath, width, height, PixelFormat::YUY2, fps)) {
        std::cerr << "[-] Помилка ініціалізації: " << sink.GetLastError() << "\n";
        std::cerr << "Підказка: Переконайтеся, що модуль v4l2loopback завантажено:\n"
                  << "  sudo modprobe v4l2loopback exclusive_caps=1 card_label=\"EyeCam Linux\"\n";
        return 1;
    }
    std::cout << "[+] Пристрій успішно відкрито та налаштовано!\n";

    std::vector<uint8_t> frameBuffer;

    // 2. Генерація 150 кадрів у форматі 1080p @ 60 FPS
    std::cout << "[2] Стрімінг 150 кадрів 1080p @ 60 FPS...\n";
    for (uint32_t i = 0; i < 150; ++i) {
        GenerateYUY2Frame(frameBuffer, width, height, i);
        if (!sink.WriteFramePaced(frameBuffer.data(), frameBuffer.size())) {
            std::cerr << "[-] Помилка запису кадру #" << i << ": " << sink.GetLastError() << "\n";
            break;
        }
        if (i % 30 == 0) {
            std::cout << "    Передано кадр #" << i << " (1080p, 60 FPS)\n";
        }
    }

    // 3. Тест динамічної зміни роздільної здатності на льоту (Dynamic Resizing)
    width = 1280;
    height = 720;
    fps = 30;
    std::cout << "\n[3] Тест динамічної зміни роздільної здатності -> " 
              << width << "x" << height << " @ " << fps << " FPS...\n";

    if (!sink.SetFormat(width, height, PixelFormat::YUY2, fps)) {
        std::cerr << "[-] Помилка динамічної зміни формату: " << sink.GetLastError() << "\n";
    } else {
        std::cout << "[+] Геометрію успішно оновлено на льоту без перезапуску пристрою!\n";

        // Передача ще 100 кадрів у новій роздільній здатності
        for (uint32_t i = 0; i < 100; ++i) {
            GenerateYUY2Frame(frameBuffer, width, height, i);
            if (!sink.WriteFramePaced(frameBuffer.data(), frameBuffer.size())) {
                std::cerr << "[-] Помилка запису кадру #" << i << ": " << sink.GetLastError() << "\n";
                break;
            }
            if (i % 30 == 0) {
                std::cout << "    Передано кадр #" << i << " (720p, 30 FPS)\n";
            }
        }
    }

    sink.Close();
    std::cout << "\n[+] Тест завершено успішно. Пристрій коректно закрито.\n";
    return 0;
}
