// Host entry point. For now it only finds the game folder and says what it
// found; the game's own startup (nnMain in the decomp's MainNX64.cpp) is called
// from here once the source links (docs/ROADMAP.md, milestone 3).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

int main(int argc, char ** argv)
{
    fs::path game = D3_DEFAULT_GAME_DIR;
    if (const char * env = std::getenv("D3_GAME_DIR"))
    {
        game = env;
    }
    for (int i = 1; i + 1 < argc; i++)
    {
        if (std::strcmp(argv[i], "--game") == 0)
        {
            game = argv[i + 1];
        }
    }

    const fs::path romfs = game / "romfs";
    std::error_code ec;
    if (!fs::is_directory(romfs, ec) || fs::is_empty(romfs, ec))
    {
        std::fprintf(stderr,
                     "No game data at %s\n"
                     "Put the RomFS contents of your own copy of Diablo III 2.6.2 (Switch) there,\n"
                     "or pass --game <folder>. See README.md.\n",
                     romfs.string().c_str());
        return 1;
    }
    std::printf("Game data: %s\n", romfs.string().c_str());
    std::printf("The game does not start yet. See docs/ROADMAP.md for what is done.\n");
    return 0;
}
