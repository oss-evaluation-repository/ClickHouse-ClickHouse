#pragma once

#include <DataStreams/IProfilingBlockInputStream.h>
#include <optional>

namespace DB
{

/// This stream allows perfoming INSERT requests in which the types of
/// the target and source blocks are compatible up to nullability:
///
/// - if a target column is nullable while the corresponding source
/// column is not, we embed the source column into a nullable column;
/// - if a source column is nullable while the corresponding target
/// column is not, we extract the nested column from the source
/// while checking that is doesn't actually contain NULLs;
/// - otherwise we just perform an identity mapping.
class NullableAdapterBlockInputStream : public IProfilingBlockInputStream
{
public:
    NullableAdapterBlockInputStream(const BlockInputStreamPtr & input, const Block & src_header_, const Block & res_header_);

    String getName() const override { return "NullableAdapterBlockInputStream"; }

    Block getHeader() const override { return header; }

protected:
    Block readImpl() override;

private:
    /// Given a column of a block we have just read,
    /// how must we process it?
    enum Action
    {
        /// Do nothing.
        NONE = 0,
        /// Convert nullable column to ordinary column.
        TO_ORDINARY,
        /// Convert non-nullable column to nullable column.
        TO_NULLABLE
    };

    /// Actions to be taken for each column of a block.
    using Actions = std::vector<Action>;

private:
    /// Determine the actions to be taken using the source sample block,
    /// which describes the columns from which we fetch data inside an INSERT
    /// query, and the target sample block which contains the columns
    /// we insert data into.
    void buildActions(const Block & src_header, const Block & res_header);

private:
    Block header;
    Actions actions;
    std::vector<std::optional<String>> rename;
    bool must_transform = false;
};

}
