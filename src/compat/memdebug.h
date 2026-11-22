// curl's memory-debugging header, which is empty in release builds. The decomp
// does not carry it because the Switch build never reaches the code that includes
// it; on macOS curl's md5.c takes its Apple branch (system MD5) and asks for it.
