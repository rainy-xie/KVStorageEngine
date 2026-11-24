#include "engine.h"

#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {
class DemoDirectory {
public:
    DemoDirectory() {
        auto name = (fs::temp_directory_path() / "kv-demo-XXXXXX").string();
        if (!::mkdtemp(name.data())) {
            throw std::runtime_error("cannot create demo directory");
        }
        path = name;
    }
    ~DemoDirectory() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};

int demo() {
    DemoDirectory directory;
    const auto database = (directory.path / "store.dat").string();
    {
        StorageEngine engine(database);
        if (engine.putIfVersion(7, 0, "first") != Status::Ok) return 1;
        const auto original = engine.read(7);
        if (original.status != Status::Ok ||
            engine.putIfVersion(7, original.version, "second") != Status::Ok ||
            engine.putIfVersion(7, original.version, "stale") != Status::Conflict) {
            return 1;
        }
        std::cout << "CAS accepted the current version and rejected the stale version.\n";

        if (engine.writeBatch({Mutation::put(8, "second"), Mutation::del(7)}) != Status::Ok)
            return 1;
        const auto values = engine.multiGet({7, 8});
        if (values[0].status != Status::NotFound || values[1].value != "second") return 1;
        std::cout << "Batch moved the value from key 7 to key 8.\n";
    }
    std::promise<std::string> completion;
    auto result = completion.get_future();
    StorageEngine reopened(database);
    reopened.asyncGet(8, [&completion](std::string value) {
        completion.set_value(std::move(value));
    });
    if (result.get() != "second") return 1;
    std::cout << "Read key 8 after reopening the database.\n";
    return 0;
}
}

int main(int argc, char **argv) {
    try {
        if (argc == 1) return demo();
        if (argc == 4 && std::string(argv[1]) == "--migrate") {
            FileStore::migrateLegacy(argv[2], argv[3]);
            std::cout << "Migrated to " << argv[3] << '\n';
            return 0;
        }
        std::cerr << "Usage: " << argv[0] << " [--migrate OLD.dat NEW.dat]\n";
        return 2;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
