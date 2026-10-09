#include <QTest>
#include <QTemporaryDir>
#include "../TestRepoHelper.h"
#include "git/Repository.h"

#include <algorithm>

namespace {

// Tag names from tags(), sorted: libgit2's enumeration order is
// loose-then-packed, not alphabetical.
QStringList tagNames(gitbolt::git::Repository& repo) {
    QStringList names;
    auto tags = repo.tags();
    if (!tags.ok())
        return {QStringLiteral("<tags() failed>")};
    for (const auto& t : tags.value())
        names << QString::fromStdString(t.name);
    names.sort();
    return names;
}

} // namespace

class TestRepository : public QObject {
    Q_OBJECT

private slots:
    void testInit() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        auto result = gitbolt::git::Repository::init(tmpDir.path().toStdString());
        QVERIFY(result.ok());
        QVERIFY(!result->isBare());
        QVERIFY(result->isHeadUnborn());
    }

    void testOpen() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        auto init = gitbolt::git::Repository::init(tmpDir.path().toStdString());
        QVERIFY(init.ok());

        auto open = gitbolt::git::Repository::open(tmpDir.path().toStdString());
        QVERIFY(open.ok());
    }

    void testOpenNonExistent() {
        auto result = gitbolt::git::Repository::open("/nonexistent/path");
        QVERIFY(!result.ok());
        QCOMPARE(result.error().code(), gitbolt::git::GitErrorCode::NotFound);
    }

    // -----------------------------------------------------------------
    // tags(): `name` is the short form createTag/deleteTag take and
    // the UI shows; `fullRefName` is the ref. git_tag_foreach reports
    // the full ref, and storing that as `name` broke Delete tag.
    // -----------------------------------------------------------------
    void tagsReportShortAndFullNames() {
        gitbolt::test::TestRepo repo;
        auto head = repo.writeAndCommit("a.txt", "a\n", "initial");
        QVERIFY(head.ok());
        QVERIFY(repo.repo().createLightweightTag("v0.1", head.value()).ok());
        QVERIFY(repo.repo().createTag("v1.0", head.value(), "release").ok());
        // Slashes are part of the short name, not a prefix to strip.
        QVERIFY(repo.repo().createLightweightTag("release/2.0", head.value()).ok());

        auto tags = repo.repo().tags();
        QVERIFY(tags.ok());
        QCOMPARE(tags->size(), size_t(3));
        for (const auto& t : tags.value()) {
            QCOMPARE(QString::fromStdString(t.fullRefName),
                     QStringLiteral("refs/tags/") + QString::fromStdString(t.name));
        }
        QCOMPARE(tagNames(repo.repo()),
                 (QStringList{"release/2.0", "v0.1", "v1.0"}));

        // A lightweight tag's ref points straight at the commit.
        auto light = std::find_if(tags->begin(), tags->end(),
                                  [](const auto& t) { return t.name == "v0.1"; });
        QVERIFY(light != tags->end());
        QCOMPARE(light->targetId, head.value());
    }

    // -----------------------------------------------------------------
    // An annotated tag's ref points at the tag object, not the commit.
    // tags() used to report that object's id as `targetId` and every
    // tag as lightweight, so the sidebar tooltip showed a hash that
    // isn't in the log and never the tag's message.
    // -----------------------------------------------------------------
    void tagsReportAnnotatedDetails() {
        gitbolt::test::TestRepo repo;
        auto tagged = repo.writeAndCommit("a.txt", "a\n", "tagged");
        QVERIFY(tagged.ok());
        QVERIFY(repo.repo().createTag("v1.0", tagged.value(), "release notes").ok());
        QVERIFY(repo.repo().createLightweightTag("v0.1", tagged.value()).ok());
        // HEAD moves on, so a tag can't pass by pointing at HEAD.
        QVERIFY(repo.writeAndCommit("b.txt", "b\n", "later").ok());
        auto tagObject = repo.repo().resolveRef("refs/tags/v1.0");
        QVERIFY(tagObject.ok());
        QVERIFY(tagObject.value() != tagged.value());

        auto tags = repo.repo().tags();
        QVERIFY(tags.ok());
        QCOMPARE(tags->size(), size_t(2));
        auto byName = [&](const std::string& name) {
            return std::find_if(tags->begin(), tags->end(),
                                [&](const auto& t) { return t.name == name; });
        };

        auto annotated = byName("v1.0");
        QVERIFY(annotated != tags->end());
        QCOMPARE(annotated->type, gitbolt::git::TagType::Annotated);
        QCOMPARE(annotated->targetId, tagged.value());
        QCOMPARE(annotated->tagId, tagObject.value());
        QCOMPARE(QString::fromStdString(annotated->message).trimmed(),
                 QStringLiteral("release notes"));
        QCOMPARE(annotated->tagger.name, std::string("Test User"));
        QCOMPARE(annotated->tagger.email, std::string("test@gitbolt.local"));

        auto light = byName("v0.1");
        QVERIFY(light != tags->end());
        QCOMPARE(light->type, gitbolt::git::TagType::Lightweight);
        QCOMPARE(light->targetId, tagged.value());
        QVERIFY(light->tagId.isZero());
        QVERIFY(light->message.empty());
        QVERIFY(light->tagger.name.empty());
    }

    // -----------------------------------------------------------------
    // deleteTag accepts the short name and the full ref name, for
    // both tag kinds, and leaves every other tag alone.
    // -----------------------------------------------------------------
    void deleteTag_data() {
        QTest::addColumn<bool>("annotated");
        QTest::addColumn<QString>("spec");

        QTest::newRow("lightweight, short name") << false << "v1.0";
        QTest::newRow("lightweight, full ref")   << false << "refs/tags/v1.0";
        QTest::newRow("annotated, short name")   << true  << "v1.0";
        QTest::newRow("annotated, full ref")     << true  << "refs/tags/v1.0";
    }

    void deleteTag() {
        QFETCH(bool, annotated);
        QFETCH(QString, spec);

        gitbolt::test::TestRepo repo;
        auto head = repo.writeAndCommit("a.txt", "a\n", "initial");
        QVERIFY(head.ok());
        auto created = annotated
            ? repo.repo().createTag("v1.0", head.value(), "release")
            : repo.repo().createLightweightTag("v1.0", head.value());
        QVERIFY(created.ok());
        QVERIFY(repo.repo().createLightweightTag("keep", head.value()).ok());
        QCOMPARE(tagNames(repo.repo()), (QStringList{"keep", "v1.0"}));

        auto result = repo.repo().deleteTag(spec.toStdString());
        QVERIFY2(result.ok(), result.error().message().c_str());
        QCOMPARE(tagNames(repo.repo()), QStringList{"keep"});
        QVERIFY(!repo.repo().resolveRef("refs/tags/v1.0").ok());
    }

    // -----------------------------------------------------------------
    // Deleting a tag that doesn't exist is a NotFound error, not a
    // crash or a silent success, and touches nothing else.
    // -----------------------------------------------------------------
    void deleteMissingTagFailsCleanly() {
        gitbolt::test::TestRepo repo;
        auto head = repo.writeAndCommit("a.txt", "a\n", "initial");
        QVERIFY(head.ok());
        QVERIFY(repo.repo().createLightweightTag("keep", head.value()).ok());

        for (const char* spec : {"nope", "refs/tags/nope"}) {
            auto result = repo.repo().deleteTag(spec);
            QVERIFY2(!result.ok(), spec);
            QCOMPARE(result.error().code(), gitbolt::git::GitErrorCode::NotFound);
            QVERIFY(!result.error().message().empty());
        }
        QCOMPARE(tagNames(repo.repo()), QStringList{"keep"});
    }

    // -----------------------------------------------------------------
    // A tag and a local branch may share a name; deleting the tag
    // must leave the branch alone.
    // -----------------------------------------------------------------
    void deleteTagSparesSameNamedBranch() {
        gitbolt::test::TestRepo repo;
        auto head = repo.writeAndCommit("a.txt", "a\n", "initial");
        QVERIFY(head.ok());
        QVERIFY(repo.repo().createLightweightTag("v1", head.value()).ok());
        QVERIFY(repo.repo().createBranch("v1", head.value()).ok());

        QVERIFY(repo.repo().deleteTag("v1").ok());
        QVERIFY(tagNames(repo.repo()).isEmpty());
        QVERIFY(repo.repo().resolveRef("refs/heads/v1").ok());
    }

    // -----------------------------------------------------------------
    // checkout of a name that is both a local branch and a tag follows
    // git's `git checkout <name>`: the branch wins, for HEAD AND for
    // the files. It used to attach HEAD to the branch but check out
    // the tag's tree, leaving phantom changes. Full refs check out
    // what they name; like git, only a bare branch name attaches HEAD.
    // -----------------------------------------------------------------
    void checkoutNameSharedByBranchAndTag_data() {
        QTest::addColumn<bool>("annotated");
        QTest::addColumn<QString>("spec");
        QTest::addColumn<QString>("expectedBranch");   // empty: detached
        QTest::addColumn<QString>("expectedContent");

        for (const bool annotated : {true, false}) {
            const char* kind = annotated ? "annotated" : "lightweight";
            QTest::addRow("%s tag, bare name", kind)
                << annotated << "v1" << "v1" << "on branch\n";
            QTest::addRow("%s tag, refs/tags/", kind)
                << annotated << "refs/tags/v1" << "" << "at tag\n";
            QTest::addRow("%s tag, refs/heads/", kind)
                << annotated << "refs/heads/v1" << "" << "on branch\n";
            QTest::addRow("%s tag, no such branch", kind)
                << annotated << "only-tag" << "" << "at tag\n";
        }
    }

    void checkoutNameSharedByBranchAndTag() {
        QFETCH(bool, annotated);
        QFETCH(QString, spec);
        QFETCH(QString, expectedBranch);
        QFETCH(QString, expectedContent);

        gitbolt::test::TestRepo repo;
        auto atTag = repo.writeAndCommit("a.txt", "at tag\n", "tagged");
        QVERIFY(atTag.ok());
        for (const char* name : {"v1", "only-tag"}) {
            auto created = annotated
                ? repo.repo().createTag(name, atTag.value(), "release")
                : repo.repo().createLightweightTag(name, atTag.value());
            QVERIFY(created.ok());
        }
        auto onBranch = repo.writeAndCommit("a.txt", "on branch\n", "branch tip");
        QVERIFY(onBranch.ok());
        QVERIFY(repo.repo().createBranch("v1", onBranch.value()).ok());
        // HEAD's own branch moves on, so neither target is current.
        QVERIFY(repo.writeAndCommit("a.txt", "on main\n", "main tip").ok());

        auto result = repo.repo().checkout(spec.toStdString());
        QVERIFY2(result.ok(), result.ok() ? "" : result.error().message().c_str());

        if (expectedBranch.isEmpty()) {
            QVERIFY(repo.repo().isHeadDetached());
        } else {
            QVERIFY(!repo.repo().isHeadDetached());
            auto branch = repo.repo().headBranchName();
            QVERIFY(branch.ok());
            QCOMPARE(QString::fromStdString(branch.value()), expectedBranch);
        }
        auto head = repo.repo().head();
        QVERIFY(head.ok());
        QCOMPARE(head.value(), expectedContent == QStringLiteral("at tag\n")
                                   ? atTag.value() : onBranch.value());

        // The files match HEAD: nothing staged, nothing modified.
        QFile file(QDir(repo.path()).absoluteFilePath(QStringLiteral("a.txt")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(file.readAll()), expectedContent);
        auto status = repo.repo().status();
        QVERIFY(status.ok());
        QCOMPARE(status->size(), size_t(0));
    }
};

QTEST_MAIN(TestRepository)
#include "TestRepository.moc"
