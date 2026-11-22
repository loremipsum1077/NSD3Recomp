// Host entry point: checks that the game's data is present, then runs the game's
// own startup exactly as the Switch's runtime would.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

#ifdef D3_WITH_GAME
extern "C" void nninitStartup();    // the game's early setup (MainNX64.cpp)
extern "C" void nnMain();           // the game's main
void d3_set_host_args(int argc, char ** argv);
#endif

int main(int argc, char ** argv)
{
    fs::path game = D3_DEFAULT_GAME_DIR;
    if (const char * env = std::getenv("D3_GAME_DIR"))
    {
        game = env;
    }

    const fs::path romfs = game / "romfs";
    std::error_code ec;
    if (!fs::is_directory(romfs, ec) || fs::is_empty(romfs, ec))
    {
        std::fprintf(stderr,
                     "No game data at %s\n"
                     "Put your own dump of Diablo III 2.6.2 (Switch) there, or set D3_GAME_DIR.\n"
                     "See README.md.\n",
                     romfs.string().c_str());
        return 1;
    }

#ifdef D3_WITH_GAME
    // Arguments are passed through to the game, as the Switch's host tools do.
    d3_set_host_args(argc, argv);
    nninitStartup();
    nnMain();
    return 0;
#else
    (void)argc;
    (void)argv;
    std::printf("Game data: %s\n", romfs.string().c_str());
    std::printf("This build has no game code (no dump was present when it was configured).\n");
    return 0;
#endif
}
