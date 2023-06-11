
#include <boost/program_options.hpp>
#include <boost/uuid/detail/sha1.hpp>
#include <iostream>
#include <sstream>

#include "git_objects/GitIndex.hpp"
#include "git_objects/GitObject.hpp"
#include "git_objects/GitObjectsFactory.hpp"
#include "git_objects/GitRepository.hpp"

#include "utilities/SHA1.hpp"
#include "utilities/Zlib.hpp"
#include <unordered_map>

void catFile(const std::string& objectFormat,
             const std::string& objectReference)
{
    auto objectHash = Git::GitObject::findObject(objectReference, objectFormat);
    auto object = GitObjectFactory::read(objectHash);
    std::cout << object->serialize().data();
}

GitHash hashFile(const std::filesystem::path& path, const std::string& format,
                 bool write = true)
{
    auto fileContent = Utilities::readFile(path);
    auto gitObject = GitObjectFactory::create(format, fileContent);

    auto objectHash = Git::GitObject::write(gitObject.get(), write);
    return objectHash;
}

// TODO: figure out when commit can have multiple parents
void displayLog(const GitHash& hash)
{
    auto gitObject = GitObjectFactory::read(hash);
    assert(gitObject->format() == "commit");
    GitCommit* commit = static_cast<GitCommit*>(gitObject.get());

    auto& commitMessage = commit->commitMessage();

    auto authorEnds = commitMessage.author.find_last_of('>');
    if (authorEnds == std::string::npos) {
        GENERATE_EXCEPTION("Malformed date in {}, date should consist of time "
                           "since epoch followed by UTC offset",
                           commitMessage.author);
    }

    auto author = commitMessage.author.substr(0, authorEnds + 1);
    auto date =
        Utilities::decodeDateIn(commitMessage.author.substr(authorEnds + 2));
    std::cout << "commit: " << hash.data().data() << std::endl;
    std::cout << "Author: " << author << std::endl;
    std::cout << "Date:   " << date << std::endl;
    std::cout << "\n\t" << commitMessage.message << std::endl;

    if (commitMessage.parent.empty()) {
        return;
    }
    else {
        displayLog(GitHash(commitMessage.parent));
    }
}

void listTree(const GitHash& objectHash, const std::string& parentDir,
              bool recursive)
{
    auto gitObject = GitObjectFactory::read(objectHash);
    auto tree = static_cast<GitTree*>(gitObject.get());

    for (const auto& treeLeaf : tree->tree()) {
        try {
            auto childFormat = GitObjectFactory::read(treeLeaf.hash)->format();
            if (recursive && childFormat == "tree") {
                listTree(treeLeaf.hash, treeLeaf.filePath.filename(),
                         recursive);
            }
            else {
                std::cout << fmt::format(
                    "{0} {1} {2}\t{3}\n", treeLeaf.fileMode, childFormat,
                    treeLeaf.hash.data(),
                    (parentDir / treeLeaf.filePath).string());
            }
        }
        catch (std::runtime_error e) {
            std::cout << e.what() << std::endl;
        }
    }
}

void treeCheckout(const GitObject* object,
                  const std::filesystem::path& checkoutDirectory)
{
    auto treeObject = dynamic_cast<const GitTree*>(object);
    for (const auto& treeLeaf : treeObject->tree()) {
        auto childObject = GitObjectFactory::read(treeLeaf.hash);
        auto destination = checkoutDirectory / treeLeaf.filePath;

        if (childObject->format() == "tree") {
            std::filesystem::create_directories(destination);
            treeCheckout(childObject.get(), destination);
        }
        else if (childObject->format() == "blob") {
            Utilities::writeToFile(destination,
                                   childObject->serialize().data());
        }
    }
}

void checkout(const GitHash& commit,
              const std::filesystem::path& checkoutDirectory)
{
    auto gitObject =
        GitObjectFactory::read(GitObject::findObject(commit.data()));

    if (std::filesystem::exists(checkoutDirectory)) {
        if (!std::filesystem::is_directory(checkoutDirectory)) {
            GENERATE_EXCEPTION("{}, is not a directory!",
                               checkoutDirectory.string());
        }
        else if (!std::filesystem::is_empty(checkoutDirectory)) {
            GENERATE_EXCEPTION(
                "{}, is not an empty directory. Currently checkout only works "
                "with the empty directories",
                checkoutDirectory.string());
        }
    }
    else {
        std::filesystem::create_directories(checkoutDirectory);
    }

    if (gitObject->format() == "commit") {
        auto gitCommit = static_cast<GitCommit*>(gitObject.get());
        auto treeHash = GitHash(gitCommit->commitMessage().tree);
        auto tree = GitObjectFactory::read(treeHash);
        treeCheckout(tree.get(), checkoutDirectory);
    }
    else if (gitObject->format() == "tree") {
        treeCheckout(gitObject.get(), checkoutDirectory);
    }
}

std::unordered_map<std::string, std::vector<std::filesystem::path>>
showReferences(const std::filesystem::path& refDir)
{
    std::unordered_map<std::string, std::vector<std::filesystem::path>> refs;
    for (auto const& dir_entry :
         std::filesystem::recursive_directory_iterator{refDir}) {
        if (dir_entry.is_regular_file()) {
            auto hash = GitObject::resolveReference(dir_entry.path());
            refs[hash].push_back(dir_entry.path());
        }
    }
    return refs;
}

void creatReference(const std::string& name, const GitHash& hash)
{
    auto repo = GitRepository::findRoot();
    auto referencePath = GitRepository::repoFile(repo, "refs", "tags", name);
    Utilities::writeToFile(referencePath, hash.data() + "\n");
}

void createTag(const std::string& tagName, const GitHash& objectHash,
               bool createAssociativeTag)
{
    if (createAssociativeTag) {
        TagMessage tagMessage{
            .object = objectHash.data(),
            .type = "commit",
            .tag = tagName,
            .tagger = "Joe Doe <joedoe@email.com>",
            .gpgsig = "",
            .message =
                "A tag generated by wyag, which won't let you customize the "
                "message!"};
        auto tag = GitTag(tagMessage);
        auto tagSHA = GitObject::write(&tag);
        creatReference(tagName, tagSHA);
    }
    else {
        creatReference(tagName, objectHash);
    }
}

void listFiles()
{
    auto repo = GitRepository::findRoot();
    auto indexFile = GitRepository::repoFile(repo, "index");

    auto res = GitIndex::parse(indexFile);
    for (const auto& entry : res) {
        std::cout << entry.objectName << std::endl;
    }
}

GitHash createTree(const std::filesystem::path& dirPath)
{
    std::vector<GitTreeLeaf> leaves;
    for (auto dirEntry : std::filesystem::directory_iterator(dirPath)) {
        auto dirEntryPath = dirEntry.path();
        if (dirEntry.is_regular_file()) {
            leaves.push_back({.fileMode = GitTree::fileMode(dirEntry, "blob"),
                              .filePath = dirEntryPath.filename(),
                              .hash = hashFile(dirEntry.path(), "blob")});
        }
        else if (dirEntry.is_directory() && !dirPath.empty() &&
                 !dirEntryPath.string().ends_with(".git")) {
            // TODO: add support for the commit(submodules)
            leaves.push_back({.fileMode = GitTree::fileMode(dirEntry, "tree"),
                              .filePath = dirEntry,
                              .hash = createTree(dirEntryPath)});
        }
    }

    GitTree tree(leaves);
    auto treeHash = GitObject::write(&tree);
    return treeHash;
}

void commit(const std::string& message = "")
{
    auto workTree = GitRepository::findRoot().workTree();
    if (workTree.empty()) {
        std::cout << "There is nothing to commit" << std::endl;
    }
    else {
        std::cout << "Create tree object: " << createTree(workTree)
                  << std::endl;
    }
}

namespace po = boost::program_options;
int main(int argc, char* argv[])
{
    po::options_description desc("Allowed options");
    // TODO: add more robust checks and meaningful description
    // clang-format off
    desc.add_options()
        ("help",
            "Produce help message"
        )
        ("init",
            po::value<std::string>()->implicit_value("."),
            "Create an empty Git repository"
        )
        ("cat-file",
            po::value<std::vector<std::string>>()->multitoken()->composing(),
            "Provide content of repository objects"
        )
        ("hash-file",
            po::value<std::vector<std::string>>()->multitoken()->composing(),
            "Compute object ID and optionally creates a blob from a file"
        )
        ("log",
            po::value<std::string>()->implicit_value("HEAD"),
            "Display history of a given commit."
        )
        ("ls-tree",
            po::value<std::vector<std::string>>()->multitoken()->composing(),
            "Pretty-print a tree object."
        )
        ("checkout",
            po::value<std::vector<std::string>>()->multitoken()->composing(),
            "Checkout a commit inside of a directory."
        )
        ("show-ref",
            po::value<std::string>()->implicit_value(".git/refs"),
            "List references."
        )
        // TODO: there is no ls-tag, make it part of tag somehow
        ("ls-tag",
            po::value<std::string>()->implicit_value(""),
            "List tags"
        )
        ("tag",
            po::value<std::vector<std::string>>()->multitoken()->composing(),
            "Create tag, use -a to create a tag object"
        )
        ("rev-parse",
            po::value<std::vector<std::string>>()->multitoken()->composing(),
            "Parse revision (or other objects )identifiers"
        )
        ("ls-files",
            po::value<std::string>()->implicit_value(""),
            "List all the stage files"
        )
        ("commit",
            po::value<std::string>()->implicit_value("."),
            "Create commit"
        )
        ;
    // clang-format on

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, desc), vm);
    po::notify(vm);

    if (vm.count("help") || argc < 2) {
        std::cout << desc << "\n";
        return 1;
    }

    try {
        if (vm.count("init")) {
            std::string pathToGitRepository = vm["init"].as<std::string>();
            auto repository = GitRepository::initialize(pathToGitRepository);
        }
        else if (vm.count("cat-file")) {
            auto catFileArguments =
                vm["cat-file"].as<std::vector<std::string>>();
            if (catFileArguments.size() == 2) {
                catFile(catFileArguments[0], catFileArguments[1]);
            }
        }
        else if (vm.count("hash-file")) {
            auto hashFileArguments =
                vm["hash-file"].as<std::vector<std::string>>();
            if (hashFileArguments.size() == 2) {
                // TODO: make write optional argument
                hashFile(hashFileArguments[0], hashFileArguments[1], true);
            }
        }
        else if (vm.count("log")) {
            auto commitHash = vm["log"].as<std::string>();
            auto object = GitObject::findObject(commitHash);
            displayLog(object);
        }
        else if (vm.count("ls-tree")) {
            auto lsTreeArguments = vm["ls-tree"].as<std::vector<std::string>>();
            auto objectHash = GitObject::findObject(lsTreeArguments[0], "tree");
            auto recursive =
                lsTreeArguments.size() > 1 && lsTreeArguments[1] == "r";
            listTree(objectHash, "", recursive);
        }
        else if (vm.count("checkout")) {
            auto checkoutArguments =
                vm["checkout"].as<std::vector<std::string>>();
            if (checkoutArguments.size() == 2) {
                auto hash = GitObject::findObject(checkoutArguments[0]);
                auto checkoutDir =
                    std::filesystem::absolute(checkoutArguments[1]);
                checkout(hash, checkoutDir);
            }
        }
        else if (vm.count("show-ref")) {
            auto path = vm["show-ref"].as<std::string>();
            for (const auto& [hash, refs] : showReferences(path)) {
                for (const auto& ref : refs) {
                    std::cout << hash << '\t' << ref.string() << std::endl;
                }
            }
        }
        else if (vm.count("ls-tag")) {
            auto tagsPath = GitRepository::repoFile(GitRepository::findRoot(),
                                                    "refs", "tags");
            if (!tagsPath.empty()) {
                for (const auto& [_, tags] : showReferences(tagsPath)) {
                    for (const auto& tag : tags) {
                        std::cout << tag.filename().string() << std::endl;
                    }
                }
            }
        }
        else if (vm.count("tag")) {
            auto tagArguments = vm["tag"].as<std::vector<std::string>>();
            if (tagArguments.size() >= 2) {
                auto tagName = tagArguments[0];
                auto objectHash = GitObject::findObject(tagArguments[1]);
                bool createAssociativeTag =
                    tagArguments.size() == 3 && tagArguments[2] == "a";
                createTag(tagName, objectHash, createAssociativeTag);
            }
        }
        else if (vm.count("rev-parse")) {
            auto revParseArgs = vm["rev-parse"].as<std::vector<std::string>>();
            if (revParseArgs.size() >= 1) {
                auto objectName = revParseArgs[0];
                auto fmt = revParseArgs.size() > 1 ? revParseArgs[1] : "";
                std::cout << GitObject::findObject(objectName, fmt)
                          << std::endl;
            }
        }
        else if (vm.count("ls-files")) {
            listFiles();
        }
        else if (vm.count("commit")) {
            commit();
        }
    }
    catch (std::runtime_error myex) {
        std::cout << myex.what() << std::endl;
    }
    return 0;
}