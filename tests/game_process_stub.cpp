// Test-only process for the Windows process guard; never launches the game.
#include <chrono>
#include <thread>

int main()
{
    std::this_thread::sleep_for(std::chrono::seconds(60));
}
