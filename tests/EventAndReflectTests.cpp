#include <CloverNT/API/Events/Event.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/API/Reflect/Reflect.hpp>
#include <CloverNT/Core/Modules/EventRuntime.hpp>
#include <CloverNT/Core/Modules/Manager.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace CloverNT::Plugin {
auto PluginManager::currentPlugin() -> std::shared_ptr<Plugin> {
    return nullptr;
}

auto Plugin::name() const noexcept -> std::string_view {
    return {};
}
} // namespace CloverNT::Plugin

namespace CloverNT::Core::Event {
struct PacketBridgeAccess {
    [[nodiscard]] static constexpr auto token() noexcept -> CloverNT::Event::KernelPublishToken {
        return CloverNT::Event::KernelPublishToken{};
    }
};
} // namespace CloverNT::Core::Event

namespace {

int gAssertions = 0;

void checkImpl(const bool cond, const char* expr, const char* file, const int line) {
    ++gAssertions;
    if (!cond) {
        std::cerr << "[FAIL] " << file << ":" << line << " assertion failed: " << expr << "\n";
        throw std::runtime_error(std::string("Assertion failed: ") + expr);
    }
}

#define CHECK(expr) checkImpl(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

enum class SpeedMode : std::int32_t {
    Eco   = 0,
    Turbo = 1,
};

struct NestedPoint {
    int    x{};
    double y{};
};

struct ComplexAggregate {
    std::int32_t                          id{};
    std::string                           label;
    CloverNT::Reflect::BridgeValue::Bytes payload;
    SpeedMode                             mode{SpeedMode::Eco};
    NestedPoint                           origin{};
    std::optional<std::string>            note;
    std::variant<std::int64_t, double>    metric{std::int64_t{0}};
    std::vector<int>                      scores;
    std::map<std::string, int>            tags;
    std::function<int(int)>               transform;
};

class Animal {
public:
    explicit Animal(std::string speciesName) : species(std::move(speciesName)) {}

    virtual ~Animal() = default;

    [[nodiscard]] virtual auto speak() const -> std::string {
        return species;
    }

    std::string species;
};

class Dog : public Animal {
public:
    inline static std::atomic<int> liveCount{0};

    Dog(std::string dogName, const int dogAge) : Animal("canine"), name(std::move(dogName)), age(dogAge) {
        liveCount.fetch_add(1, std::memory_order_relaxed);
    }

    ~Dog() override {
        liveCount.fetch_sub(1, std::memory_order_relaxed);
    }

    [[nodiscard]] auto speak() const -> std::string override {
        return name + ":woof";
    }

    [[nodiscard]] auto energy() const noexcept -> int {
        return mEnergy;
    }

    void setEnergy(const int v) noexcept {
        mEnergy = v;
    }

    [[nodiscard]] auto barkOnce() const -> std::string {
        return "bark!";
    }

    [[nodiscard]] auto barkMany(const int times) const -> std::string {
        std::string out;
        for (int i = 0; i < times; ++i) {
            out += "w";
        }
        return out;
    }

    void birthday() noexcept {
        ++age;
    }

    std::string name;
    int         age{};

private:
    int mEnergy{100};
};

struct IJsStore {
    int version{};

    auto get(std::string key) -> std::string {
        return key;
    }

    void put(std::string, std::string) {}
};

} // namespace

template <>
struct CloverNT::Reflect::Class<Animal> {
    static constexpr bool isReflected = true;
    static constexpr auto members =
            Reflect::members(Reflect::prop<&Animal::species>(readonly), Reflect::method<&Animal::speak>());
};

template <>
struct CloverNT::Reflect::Class<Dog> {
    static constexpr bool isReflected = true;
    using Base                        = Animal;
    static constexpr auto members     = Reflect::members(Reflect::ctor<std::string, int>(),
                                                         Reflect::prop<&Dog::name>(),
                                                         Reflect::prop<&Dog::age>(),
                                                         Reflect::prop<"energy", &Dog::energy, &Dog::setEnergy>(),
                                                         Reflect::method<&Dog::birthday>(),
                                                         Reflect::overload<"bark", &Dog::barkOnce, &Dog::barkMany>());
};

template <>
struct CloverNT::Reflect::Interface<IJsStore> {
    static constexpr bool isReflected = true;
    static constexpr auto members     = Reflect::members(
            Reflect::prop<&IJsStore::version>(), Reflect::method<&IJsStore::get>(), Reflect::method<&IJsStore::put>());
};

namespace {

using namespace CloverNT;

class MockMethodCallable final : public Reflect::BridgeCallableImpl {
public:
    using Fn = std::function<Expected<Reflect::BridgeValue>(std::span<const Reflect::BridgeValue>)>;

    explicit MockMethodCallable(Fn fn) : mFn(std::move(fn)) {}

    [[nodiscard]] auto ownerPlugin() const noexcept -> std::string_view override {
        return "mock-js";
    }

    [[nodiscard]] bool canInvokeSyncHere() const noexcept override {
        return true;
    }

    [[nodiscard]] auto invoke(const std::span<const Reflect::BridgeValue> args)
            -> Expected<Reflect::BridgeValue> override {
        return mFn(args);
    }

    [[nodiscard]] auto post(std::vector<Reflect::BridgeValue> args) -> Expected<void> override {
        auto r = mFn(args);
        if (!r) {
            return unexpected(std::move(r).error());
        }
        return {};
    }

    [[nodiscard]] auto invokeAsync(std::vector<Reflect::BridgeValue> args) -> Reflect::BridgeAsync override {
        auto st = std::make_shared<Reflect::BridgeAsyncState>();
        st->settle(mFn(args));
        return st;
    }

private:
    Fn mFn;
};

class MockJsObject final : public Reflect::RemoteObjectImpl {
public:
    std::unordered_map<std::string, Reflect::BridgeValue> props;
    std::unordered_map<std::string,
                       std::function<Expected<Reflect::BridgeValue>(std::span<const Reflect::BridgeValue>)>>
            methods;

    [[nodiscard]] bool canAccessSyncHere() const noexcept override {
        return true;
    }

    [[nodiscard]] auto hasProperty(const std::string_view key) -> Expected<bool> override {
        const std::string k{key};
        return props.contains(k) || methods.contains(k);
    }

    [[nodiscard]] auto getProperty(const std::string_view key) -> Expected<Reflect::BridgeValue> override {
        const std::string k{key};
        if (const auto it = props.find(k); it != props.end()) {
            return it->second;
        }
        if (const auto mit = methods.find(k); mit != methods.end()) {
            return Reflect::BridgeValue{std::static_pointer_cast<Reflect::BridgeCallableImpl>(
                    std::make_shared<MockMethodCallable>(mit->second))};
        }
        return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::MemberNotFound, "missing prop");
    }

    [[nodiscard]] auto setProperty(const std::string_view key, Reflect::BridgeValue value) -> Expected<void> override {
        props[std::string(key)] = std::move(value);
        return {};
    }

    [[nodiscard]] auto propertyNames() -> Expected<std::vector<std::string>> override {
        std::vector<std::string> out;
        out.reserve(props.size());
        for (const auto& k: props | std::views::keys) {
            out.push_back(k);
        }
        return out;
    }

    [[nodiscard]] auto invokeMethod(const std::string_view method, const std::span<const Reflect::BridgeValue> args)
            -> Expected<Reflect::BridgeValue> override {
        const auto it = methods.find(std::string(method));
        if (it == methods.end()) {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::MemberNotFound, "missing method");
        }
        return it->second(args);
    }

    [[nodiscard]] auto postMethod(const std::string_view method, std::vector<Reflect::BridgeValue> args)
            -> Expected<void> override {
        auto r = invokeMethod(method, args);
        if (!r) {
            return unexpected(std::move(r).error());
        }
        return {};
    }

    [[nodiscard]] auto invokeMethodAsync(const std::string_view method, std::vector<Reflect::BridgeValue> args)
            -> Reflect::BridgeAsync override {
        auto state = std::make_shared<Reflect::BridgeAsyncState>();
        state->settle(invokeMethod(method, args));
        return state;
    }

    [[nodiscard]] auto getPropertyAsync(const std::string_view key) -> Reflect::BridgeAsync override {
        auto state = std::make_shared<Reflect::BridgeAsyncState>();
        state->settle(getProperty(key));
        return state;
    }
};

class MockDynamicJsSlot final : public Event::ListenerSlot {
public:
    using Handler = std::function<void(Event::EventInvocation&)>;

    MockDynamicJsSlot(const Event::SubscriptionId id,
                      const Event::EventPriority  prio,
                      const std::string&          owner,
                      Handler                     fn)
        : ListenerSlot(id, prio, /*once=*/false, std::string(owner), Reflect::acquirePluginActivity(owner)),
          mFn(std::move(fn)) {}

    void dispatch(Event::EventInvocation& inv) override {
        mFn(inv);
    }

private:
    Handler mFn;
};

void testValueAndAggregateMarshaling() {
    static_assert(Reflect::ReflectedAggregate<ComplexAggregate>);
    static_assert(Reflect::aggregateArity<ComplexAggregate> == 10);
    constexpr auto names = Reflect::aggregateFieldNames<ComplexAggregate>;
    static_assert(names[0] == "id");
    static_assert(names[1] == "label");
    static_assert(names[2] == "payload");
    static_assert(names[9] == "transform");

    ComplexAggregate src{
            .id        = 99,
            .label     = "alpha",
            .payload   = {0xDE, 0xAD, 0xBE, 0xEF},
            .mode      = SpeedMode::Turbo,
            .origin    = NestedPoint{.x = -7, .y = 3.5},
            .note      = "hello-reflect",
            .metric    = 42.25,
            .scores    = {10, 20, 30},
            .tags      = {{"k1", 1}, {"k2", 2}},
            .transform = [](const int v) { return v * 3 + 1; },
    };

    Reflect::MarshalContext ctx{};
    auto                    bridged = Reflect::toBridge(src, ctx);
    CHECK(bridged.has_value());
    CHECK(bridged->isObject());

    auto restored = Reflect::fromBridge<ComplexAggregate>(*bridged, ctx);
    CHECK(restored.has_value());
    CHECK(restored->id == 99);
    CHECK(restored->label == "alpha");
    CHECK((restored->payload == Reflect::BridgeValue::Bytes{0xDE, 0xAD, 0xBE, 0xEF}));
    CHECK(restored->mode == SpeedMode::Turbo);
    CHECK(restored->origin.x == -7);
    CHECK(restored->origin.y == 3.5);
    CHECK(restored->note.has_value() && *restored->note == "hello-reflect");
    CHECK(std::holds_alternative<double>(restored->metric));
    CHECK(std::get<double>(restored->metric) == 42.25);
    CHECK((restored->scores == std::vector<int>{10, 20, 30}));
    CHECK(restored->tags.at("k2") == 2);
    CHECK(static_cast<bool>(restored->transform));
    CHECK(restored->transform(10) == 31);

    const Reflect::BridgeValue hugeInt{std::int64_t{5'000'000'000LL}};
    auto                       narrow = Reflect::fromBridge<std::int32_t>(hugeInt, ctx);
    CHECK(!narrow.has_value());
    CHECK(narrow.error().code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::IntegerOverflow));
}

void testSharedObjectAndProxy() {
    CHECK(Dog::liveCount.load() == 0);

    Reflect::MarshalContext ctx{};
    Reflect::BridgeValue    escapedCell;

    {
        const Reflect::ScopedOwnerPlugin ownerScope{"plugin-dog"};
        const auto*                      desc = Reflect::classDescriptorOf<Dog>();
        CHECK(desc != nullptr);
        CHECK(!desc->ctors.empty());

        std::vector ctorArgs{
                Reflect::BridgeValue{std::string("Rex")},
                Reflect::BridgeValue{std::int64_t{4}},
        };
        auto constructedCell = desc->ctors[0].construct(ctorArgs);
        CHECK(constructedCell.has_value());
        CHECK(Dog::liveCount.load() == 1);

        escapedCell = Reflect::BridgeValue{*constructedCell};

        auto dogPtr = Reflect::fromBridge<std::shared_ptr<Dog>>(escapedCell, ctx);
        CHECK(dogPtr.has_value() && *dogPtr != nullptr);
        CHECK((*dogPtr)->name == "Rex");
        CHECK((*dogPtr)->age == 4);

        auto animalPtr = Reflect::fromBridge<std::shared_ptr<Animal>>(escapedCell, ctx);
        CHECK(animalPtr.has_value() && *animalPtr != nullptr);
        CHECK((*animalPtr)->speak() == "Rex:woof");
        CHECK(animalPtr->get() == static_cast<Animal*>(dogPtr->get()));

        std::weak_ptr<Dog> weakDog = *dogPtr;
        auto               wb      = Reflect::toBridge(weakDog, ctx);
        CHECK(wb.has_value());
        auto weakRestored = Reflect::fromBridge<std::weak_ptr<Dog>>(*wb, ctx);
        CHECK(weakRestored.has_value());
        CHECK(!weakRestored->expired());

        const auto* barkMethod = desc->findMethod("bark");
        CHECK(barkMethod != nullptr);
        auto bark0 = barkMethod->invoke(dogPtr->get(), false, {}, nullptr);
        CHECK(bark0.has_value() && std::get<std::string>(bark0->data) == "bark!");
        std::vector barkArgs{Reflect::BridgeValue{std::int64_t{3}}};
        auto        bark3 = barkMethod->invoke(dogPtr->get(), false, barkArgs, nullptr);
        CHECK(bark3.has_value() && std::get<std::string>(bark3->data) == "www");

        const auto* bdayMethod = desc->findMethod("birthday");
        CHECK(bdayMethod != nullptr);
        auto constViolation = bdayMethod->invoke(dogPtr->get(), /*isConstReceiver=*/true, {}, nullptr);
        CHECK(!constViolation.has_value());
        CHECK(constViolation.error().code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::ReadonlyViolation));
    }

    CHECK(Dog::liveCount.load() == 1);

    CHECK(Event::EventBus::clearPlugin("plugin-dog").has_value());
    CHECK(Event::EventBus::waitPluginIdle("plugin-dog").has_value());
    CHECK(Dog::liveCount.load() == 0);

    auto afterUnload = Reflect::fromBridge<std::shared_ptr<Dog>>(escapedCell, ctx);
    CHECK(!afterUnload.has_value());
    CHECK(afterUnload.error().code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::ObjectRevoked));

    auto mockJs              = std::make_shared<MockJsObject>();
    mockJs->props["version"] = Reflect::BridgeValue{std::int64_t{7}};
    mockJs->methods["get"] =
            [raw = mockJs.get()](std::span<const Reflect::BridgeValue> a) -> Expected<Reflect::BridgeValue> {
        const auto key = std::get<std::string>(a[0].data);
        const auto it  = raw->props.find("data:" + key);
        return it != raw->props.end() ? it->second : Reflect::BridgeValue{std::string("<none>")};
    };
    mockJs->methods["put"] =
            [raw = mockJs.get()](std::span<const Reflect::BridgeValue> a) -> Expected<Reflect::BridgeValue> {
        raw->props["data:" + std::get<std::string>(a[0].data)] = a[1];
        return Reflect::BridgeValue{nullptr};
    };

    const Reflect::BridgeValue remoteVal{std::static_pointer_cast<Reflect::RemoteObjectImpl>(mockJs)};
    auto                       proxy = Reflect::fromBridge<Reflect::Proxy<IJsStore>>(remoteVal, ctx);
    CHECK(proxy.has_value());
    CHECK(proxy->get<&IJsStore::version>().value() == 7);
    CHECK(proxy->set<&IJsStore::version>(8).has_value());
    CHECK(proxy->get<&IJsStore::version>().value() == 8);
    CHECK(proxy->call<&IJsStore::put>(std::string("greeting"), std::string("hi!")).has_value());
    auto got = proxy->call<&IJsStore::get>(std::string("greeting"));
    CHECK(got.has_value() && *got == "hi!");

    auto incompleteJs              = std::make_shared<MockJsObject>();
    incompleteJs->props["version"] = Reflect::BridgeValue{std::int64_t{1}};
    const Reflect::BridgeValue badRemoteVal{std::static_pointer_cast<Reflect::RemoteObjectImpl>(incompleteJs)};
    auto                       badProxy = Reflect::fromBridge<Reflect::Proxy<IJsStore>>(badRemoteVal, ctx);
    CHECK(!badProxy.has_value());
    CHECK(badProxy.error().code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::MissingField));
}

void testScopedBorrow() {
    Dog                  stackDog{"Bolt", 2};
    Reflect::BridgeValue escapedBorrow;

    {
        Reflect::BorrowScopeGuard borrowGuard;
        Reflect::MarshalContext   ctx{.activeLease = borrowGuard.lease().get()};

        auto bv = Reflect::toBridge(Reflect::borrow(&stackDog), ctx);
        CHECK(bv.has_value());
        CHECK(bv->isBorrowedObject());
        escapedBorrow = *bv;

        auto borrowedDog = Reflect::fromBridge<Reflect::Borrowed<Dog>>(escapedBorrow, ctx);
        CHECK(borrowedDog.has_value() && static_cast<bool>(*borrowedDog));
        (*borrowedDog)->birthday();
        CHECK(stackDog.age == 3);

        auto borrowedAnimal = Reflect::fromBridge<Reflect::Borrowed<const Animal>>(escapedBorrow, ctx);
        CHECK(borrowedAnimal.has_value());
        CHECK((*borrowedAnimal)->speak() == "Bolt:woof");
    }

    Reflect::MarshalContext afterCtx{};
    auto                    expired = Reflect::fromBridge<Reflect::Borrowed<Dog>>(escapedBorrow, afterCtx);
    CHECK(!expired.has_value());
    CHECK(expired.error().code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::BorrowExpired));
}

auto asyncStepAdd(const int base) -> Reflect::Async<int> {
    auto [f1, r1] = Reflect::makeAsync<int>();
    r1.resolve(base + 10);

    const auto v1 = co_await f1;
    if (!v1) {
        co_return unexpected(v1.error());
    }

    auto [f2, r2] = Reflect::makeAsync<int>();
    r2.resolve(*v1 * 2);

    const auto v2 = co_await f2;
    co_return v2;
}

auto asyncStepFail() -> Reflect::Async<int> {
    auto [f, r] = Reflect::makeAsync<int>();
    r.reject(Reflect::bridgeError(Reflect::BridgeErrorCode::ExecutionFailed, "boom"));
    const auto v = co_await f;
    co_return v;
}

void testCallableAndAsync() {
    Reflect::MarshalContext ctx{};
    Reflect::BridgeValue    escapedFn;

    {
        const Reflect::ScopedOwnerPlugin ownerScope{"plugin-fn"};
        auto lambda = [prefix = std::string("res=")](const int a, const std::string& b) -> std::string {
            return prefix + std::to_string(a) + ":" + b;
        };
        auto bridged = Reflect::toBridge(lambda, ctx);
        CHECK(bridged.has_value());
        escapedFn = *bridged;

        auto callable = Reflect::fromBridge<Reflect::Callable<std::string(int, std::string)>>(escapedFn, ctx);
        CHECK(callable.has_value());
        CHECK((*callable)(42, "ok") == "res=42:ok");
        auto tried = callable->tryInvoke(7, "hi");
        CHECK(tried.has_value() && *tried == "res=7:hi");
    }

    CHECK(Event::EventBus::clearPlugin("plugin-fn").has_value());
    CHECK(Event::EventBus::waitPluginIdle("plugin-fn").has_value());
    auto callableAfter = Reflect::fromBridge<Reflect::Callable<std::string(int, std::string)>>(escapedFn, ctx);
    CHECK(callableAfter.has_value());
    auto revokedRes = callableAfter->tryInvoke(1, "x");
    CHECK(!revokedRes.has_value());
    CHECK(revokedRes.error().code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::ObjectRevoked));

    auto okTask    = asyncStepAdd(5);
    bool settledOk = false;
    okTask.then([&](const Expected<int>& res) {
        CHECK(res.has_value() && *res == 30);
        settledOk = true;
    });
    CHECK(settledOk);

    auto failTask    = asyncStepFail();
    bool settledFail = false;
    failTask.then([&](const Expected<int>& res) {
        CHECK(!res.has_value());
        CHECK(res.error().message == "boom");
        settledFail = true;
    });
    CHECK(settledFail);
}

void testErrorNormalizationAndNoNesting() {
    CHECK(normalizeErrorMessage("Error: Error: TypeError: invalid token") == "invalid token");
    CHECK(normalizeErrorMessage("Error invoking remote method 'clovernt:plugin-action': Error: Error: bad config") ==
          "bad config");
    CHECK(normalizeErrorMessage("   RangeError:  Error: out of bounds") == "out of bounds");
    CHECK(normalizeErrorMessage("Error:") == "Error:");
    CHECK(normalizeErrorMessage("plain failure") == "plain failure");

    const auto err = makeError(
            ErrorCategory::Plugin, CommonErrorCode::OperationFailed, "Error: TypeError: Error: nested failure");
    CHECK(err.message == "nested failure");

    Reflect::MarshalContext          ctx{};
    const Reflect::ScopedOwnerPlugin ownerScope{"plugin-err"};

    auto inner = []() -> int { throw std::runtime_error("Error: Error: disk full"); };
    auto b1    = Reflect::toBridge(inner, ctx);
    CHECK(b1.has_value());
    auto c1 = Reflect::fromBridge<std::function<int()>>(*b1, ctx);
    CHECK(c1.has_value());

    auto outer = [fn = *c1]() -> int { return fn(); };
    auto b2    = Reflect::toBridge(outer, ctx);
    CHECK(b2.has_value());
    auto c2 = Reflect::fromBridge<Reflect::Callable<int()>>(*b2, ctx);
    CHECK(c2.has_value());

    auto res = c2->tryInvoke();
    CHECK(!res.has_value());
    CHECK(res.error().message == "disk full");

    CHECK(Event::EventBus::clearPlugin("plugin-err").has_value());
    CHECK(Event::EventBus::waitPluginIdle("plugin-err").has_value());
}

void testSingleChannelEventBus() {
    constexpr auto kernelToken = Core::Event::PacketBridgeAccess::token();

    Nt::Packet emittedPkt{
            .seq        = 1,
            .command    = "trpc.test.Echo",
            .uin        = "10001",
            .uid        = "u_10001",
            .encryption = Nt::Encryption::D2Key,
            .body       = {1, 2, 3},
            .injected   = false,
    };

    const Nt::Packet* observedAddress = nullptr;
    {
        auto sub = Event::EventBus::on<Event::Events::PacketSend>(
                [&](Nt::Packet& pkt) { observedAddress = &pkt; }, Event::EventPriority::Highest, "CloverCore");
        CHECK(sub.has_value());

        auto cancelled = Event::EventBus::emit<Event::Events::PacketSend>(emittedPkt, kernelToken);
        CHECK(cancelled.has_value() && !*cancelled);
        CHECK(observedAddress == &emittedPkt);
    }

    std::vector<std::string> order;
    bool                     monitorSawCancel = false;
    std::string              monitorSawCmd;

    auto subHighest = Event::EventBus::on<Event::Events::PacketSend>(
            [&](Nt::Packet& pkt) {
                order.emplace_back("highest-cpp");
                pkt.seq = 777;
            },
            Event::EventPriority::Highest,
            "plugin-a");
    CHECK(subHighest.has_value());

    const auto dynId   = Event::EventBus::allocateSubscriptionId();
    auto       dynSlot = std::make_shared<MockDynamicJsSlot>(
            dynId, Event::EventPriority::Normal, "plugin-js", [&](Event::EventInvocation& inv) {
                order.emplace_back("normal-js");
                auto args = inv.bridgeArgs();
                CHECK(args.has_value() && args->size() == 1);

                Reflect::MarshalContext mctx{};
                auto                    seen = Reflect::fromBridge<Nt::Packet>((*args)[0], mctx);
                CHECK(seen.has_value());
                CHECK(seen->seq == 777);

                seen->command = "trpc.modified.ByJs";
                seen->body.push_back(0x99);
                auto updatedVal = Reflect::toBridge(*seen, mctx);
                CHECK(updatedVal.has_value());
                std::vector updatedVec{std::move(*updatedVal)};
                CHECK(inv.commitBridgeMutation(updatedVec).has_value());
            });
    CHECK(Event::EventBus::addSlot(Event::Events::PacketSend::topic, dynSlot).has_value());
    Event::ScopedSubscription subNormalJs{dynId};

    auto subLowCancel = Event::EventBus::on<Event::Events::PacketSend>(
            [&](const Nt::Packet& pkt, Event::InterceptEventContext<true>& ctx) {
                order.emplace_back("low-cpp-cancel");
                CHECK(pkt.seq == 777);
                CHECK(pkt.command == "trpc.modified.ByJs");
                CHECK((pkt.body == std::vector<std::uint8_t>{1, 2, 3, 0x99}));
                CHECK(ctx.cancel());
            },
            Event::EventPriority::Low,
            "plugin-b");
    CHECK(subLowCancel.has_value());

    auto subLowestSkipped = Event::EventBus::on<Event::Events::PacketSend>(
            [&](const Nt::Packet&) { order.emplace_back("lowest-should-not-run"); },
            Event::EventPriority::Lowest,
            "plugin-c");
    CHECK(subLowestSkipped.has_value());

    auto subMonitor = Event::EventBus::monitor<Event::Events::PacketSend>(
            [&](const Nt::Packet& pkt, const Event::ReadOnlyEventContext& ctx) {
                order.emplace_back("monitor");
                monitorSawCancel = ctx.isCancelled();
                monitorSawCmd    = pkt.command;
            },
            "plugin-monitor");
    CHECK(subMonitor.has_value());

    auto emitRes = Event::EventBus::emit<Event::Events::PacketSend>(emittedPkt, kernelToken);
    CHECK(emitRes.has_value() && *emitRes == true);
    CHECK((order == std::vector<std::string>{"highest-cpp", "normal-js", "low-cpp-cancel", "monitor"}));
    CHECK(monitorSawCancel);
    CHECK(monitorSawCmd == "trpc.modified.ByJs");
    CHECK(emittedPkt.seq == 777);
    CHECK(emittedPkt.command == "trpc.modified.ByJs");

    {
        const Reflect::ScopedOwnerPlugin chatScope{"chat-helper"};

        auto spoofKernel = Event::EventBus::emit("clovernt:packet-send", 123);
        CHECK(!spoofKernel.has_value());
        CHECK(spoofKernel.error().code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::PermissionDenied));

        auto spoofForeign = Event::EventBus::emit("other-plugin:secret", 123);
        CHECK(!spoofForeign.has_value());
        CHECK(spoofForeign.error().code == static_cast<std::int32_t>(Reflect::BridgeErrorCode::PermissionDenied));

        bool readyFired = false;
        auto subReady   = Event::EventBus::on("ready", [&] { readyFired = true; });
        CHECK(subReady.has_value());
        CHECK(Event::EventBus::listenerCount("chat-helper:ready") == 1);

        auto readyEmit = Event::EventBus::emit("ready");
        CHECK(readyEmit.has_value());
        CHECK(readyFired);

        auto subRpc = Event::EventBus::on(
                "chat-helper:rpc",
                [](const int                                 code,
                   std::string&                              tag,
                   const Reflect::Callable<int(int)>&        compute,
                   const Event::InterceptEventContext<true>& ctx) {
                    CHECK(ctx.sourcePlugin() == "chat-helper");
                    const auto computed = compute.tryInvoke(code);
                    CHECK(computed.has_value());
                    tag += ":" + std::to_string(*computed);
                },
                Event::EventPriority::Normal,
                "consumer-plugin");
        CHECK(subRpc.has_value());

        std::string rpcTag = "out";
        auto        rpcRes = Event::EventBus::emit("rpc", 6, rpcTag, [](const int x) { return x * 7; });
        CHECK(rpcRes.has_value() && !*rpcRes);
        CHECK(rpcTag == "out:42");
    }

    {
        const Reflect::ScopedOwnerPlugin concScope{"conc-plugin"};
        std::atomic<int>                 onceCount{0};
        auto                             subOnce =
                Event::EventBus::once("conc-plugin:ping", [&] { onceCount.fetch_add(1, std::memory_order_relaxed); });
        CHECK(subOnce.has_value());

        constexpr int            kThreads = 8;
        std::latch               startGate{kThreads};
        std::vector<std::thread> workers;
        workers.reserve(kThreads);
        for (int i = 0; i < kThreads; ++i) {
            workers.emplace_back([&] {
                const Reflect::ScopedOwnerPlugin workerScope{"conc-plugin"};
                startGate.arrive_and_wait();
                (void) Event::EventBus::emit("conc-plugin:ping");
            });
        }
        for (auto& t: workers) {
            t.join();
        }
        CHECK(onceCount.load() == 1);
    }
}

void testVoidEventTopicAndLoggerFlush() {
    static_assert(std::tuple_size_v<Event::Events::All> == 6);

    using VoidPing = Event::EventTopic<"void-plugin:ping", void, false, true, false>;
    const Reflect::ScopedOwnerPlugin ownerScope{"void-plugin"};

    int  hits        = 0;
    bool monitorSeen = false;

    auto subNormal = Event::EventBus::on<VoidPing>(
            [&](Event::InterceptEventContext<true>& ctx) {
                ++hits;
                if (hits == 2) {
                    CHECK(ctx.cancel());
                }
            },
            Event::EventPriority::Normal);
    CHECK(subNormal.has_value());

    auto subLow = Event::EventBus::on<VoidPing>([&] { ++hits; }, Event::EventPriority::Low);
    CHECK(subLow.has_value());

    auto subMon = Event::EventBus::monitor<VoidPing>(
            [&](const Event::ReadOnlyEventContext& ctx) { monitorSeen = ctx.isCancelled(); });
    CHECK(subMon.has_value());

    auto first = Event::EventBus::emit<VoidPing>();
    CHECK(first.has_value() && !*first);
    CHECK(hits == 2);
    CHECK(!monitorSeen);

    hits        = 1;
    auto second = Event::EventBus::emit<VoidPing>();
    CHECK(second.has_value() && *second);
    CHECK(hits == 2); // Low listener skipped after cancel
    CHECK(monitorSeen);

    struct ReentrantFlushConsumer final : LogConsumer {
        explicit ReentrantFlushConsumer(std::shared_ptr<Logger> logRef) : logger(std::move(logRef)) {}

        void consume(LogMessage const&, std::string_view, std::string_view) override {
            std::lock_guard lock(mutex);
            logger->flush(); // Must not deadlock or re-enter flush() on the worker thread
            ++consumed;
        }

        void flush() override {
            std::lock_guard lock(mutex);
            ++flushed;
        }

        std::shared_ptr<Logger> logger;
        std::mutex              mutex;
        std::atomic<int>        consumed{0};
        std::atomic<int>        flushed{0};
    };

    auto log = LoggerRegistry::getInstance().getOrCreate("TestReentrantFlush");
    log->setOutputConsole(false);
    auto consumer   = std::make_shared<ReentrantFlushConsumer>(log);
    auto consumerId = log->addConsumer(consumer);
    CHECK(consumerId.has_value());
    log->info("hello reentrant flush");
    log->flush();
    CHECK(consumer->consumed.load() == 1);
    CHECK(consumer->flushed.load() == 1);
    CHECK(log->removeConsumer(*consumerId).has_value());
}

} // namespace

int main() {
    try {
        auto&      mgr  = Core::Modules::Manager::getInstance();
        const auto init = mgr.initialize();
        CHECK(init.has_value());
        CHECK(mgr.registerModule<Core::Modules::EventRuntime>("EventRuntime", "Test EventRuntime").has_value());
        CHECK(mgr.loadAll().has_value());
        CHECK(mgr.enableAll().has_value());

        testValueAndAggregateMarshaling();
        testSharedObjectAndProxy();
        testScopedBorrow();
        testCallableAndAsync();
        testErrorNormalizationAndNoNesting();
        testSingleChannelEventBus();
        testVoidEventTopicAndLoggerFlush();

        CHECK(mgr.destroy().has_value());
        std::cout << "[PASS] CloverEventTests passed all " << gAssertions << " assertions.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FATAL] " << ex.what() << "\n";
        return 1;
    }
}
