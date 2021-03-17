#include "zsr-client.hpp"

#include <zsr/find.hpp>
#include <zsr/introspectable.hpp>

#include <cassert>

namespace zswagcl
{

ZsrClient::ZsrClient(zswagcl::OpenAPIConfig config,
                     std::unique_ptr<httpcl::IHttpClient> client)
    : client_(std::move(config), std::move(client))
{}

ZsrClient::~ZsrClient()
{}

template <class _Iter>
zsr::Variant queryFieldRecursive(zsr::Variant object, _Iter begin, _Iter end)
{
    if (begin == end)
        return object;

    if (auto introspectable = object.get<zsr::Introspectable>()) {
        auto meta = introspectable->meta();
        assert(meta);

        if (auto field = zsr::find<zsr::Field>(*meta, std::string(*begin)))
            return queryFieldRecursive(field->get(*introspectable), begin + 1, end);

        if (auto fun = zsr::find<zsr::Function>(*meta, std::string(*begin)))
            return queryFieldRecursive(fun->call(*introspectable), begin + 1, end);

        throw std::runtime_error(stx::replace_with("Could not find field/function for identifier '?'", "?", *begin));
    }

    throw std::runtime_error("Returned variant is not an object");
}

struct VariantVisitor
{
    ParameterValueHelper& helper;

    VariantVisitor(ParameterValueHelper& helper)
        : helper(helper)
    {}

    auto operator()()
    {
        return helper.binary({});
    }

    template <class _T>
    auto operator()(_T value)
    {
        return helper.value(std::forward<_T>(value));
    }

    auto operator()(const zserio::BitBuffer& value)
    {
        return helper.binary({value.getBuffer(), value.getBuffer() + value.getByteSize()});
    }

    auto operator()(const zsr::Introspectable& value)
    {
        auto meta = value.meta();
        assert(meta);

        std::map<std::string, Any> map;
        for (const auto& field : meta->fields) {
            assert(field.get);

            /* Skip optional fields */
            if (field.has)
                if (!field.has(value))
                    continue;

            auto fieldValue = field.get(value);
            auto read = [&]() -> Any {
                if (auto tmp = fieldValue.get<std::int64_t>())
                    return *tmp;
                if (auto tmp = fieldValue.get<std::uint64_t>())
                    return *tmp;
                if (auto tmp = fieldValue.get<double>())
                    return *tmp;
                if (auto tmp = fieldValue.get<std::string>())
                    return *tmp;

                throw std::runtime_error("Unsupported variant type");
            };

            map[field.ident] = read();
        }

        return helper.object(std::move(map));
    }

    template <class _T>
    auto operator()(const std::vector<_T>& value)
    {
        return helper.array(value);
    }

    auto operator()(const std::vector<zserio::BitBuffer>& value)
    {
        std::vector<std::string> array;
        for (const auto& buffer : value)
            array.emplace_back(buffer.getBuffer(), buffer.getBuffer() + buffer.getByteSize());

        return helper.array(std::move(array));
    }

    auto operator()(const std::vector<zsr::Introspectable>& value)
    {
        std::vector<std::string> array;
        for (auto& object : value) {
            auto meta = object.meta();
            assert(meta);
            assert(meta->bitSize);

            auto size = meta->bitSize(object);
            std::vector<std::uint8_t> buffer;
            buffer.resize((size + 7) / 8);

            zserio::BitStreamWriter writer(buffer.data(), size);
            if (meta->write)
                meta->write(const_cast<zsr::Introspectable&>(object), writer);

            array.emplace_back(buffer.begin(), buffer.end());
        }

        return helper.array(array);
    }
};

void ZsrClient::callMethod(const std::string& method,
                           const std::vector<uint8_t>& requestData,
                           std::vector<uint8_t>& responseData,
                           void* context)
{
    assert(context);

    const auto* ctx = reinterpret_cast<const zsr::ServiceMethod::Context*>(context);

    auto response = client_.call(method, [&](const std::string& parameter, const std::string& field, ParameterValueHelper& helper) {
        if (field == "" || field == "*")
            return helper.binary(requestData);

        auto parts = stx::split<std::vector<std::string_view>>(field, ".");
        auto value = queryFieldRecursive(ctx->request, parts.begin(), parts.end());

        VariantVisitor visitor(helper);
        return zsr::visit(value, visitor);
    });

    responseData.assign(response.begin(), response.end());
}

}
