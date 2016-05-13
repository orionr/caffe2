#ifndef CAFFE2_CORE_REGISTRY_H_
#define CAFFE2_CORE_REGISTRY_H_

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>

#include "caffe2/core/common.h"
#include "caffe2/core/typeid.h"

namespace caffe2 {

// Registry is a class that allows one to register classes by a specific
// key, usually a string specifying the name. For each key type and object type,
// there should be only one single registry responsible for it.

template <class SrcType, class ObjectType, class... Args>
class Registry {
 public:
  typedef std::function<ObjectType*(Args ...)> Creator;

  Registry() : registry_() {}

  void Register(const SrcType& key, Creator creator) {
    // The if statement below is essentially the same as the following line:
    // CAFFE_CHECK_EQ(registry_.count(key), 0) << "Key " << key
    //                                   << " registered twice.";
    // However, CAFFE_CHECK_EQ depends on google logging, and since registration is
    // carried out at static initialization time, we do not want to have an
    // explicit dependency on glog's initialization function.
    std::lock_guard<std::mutex> lock(register_mutex_);
    if (registry_.count(key) != 0) {
      std::cerr << "Key " << key << " already registered." << std::endl;
      std::exit(1);
    }
    //std::cout << "Registering " << key << " for "
    //          << typeid(ObjectType).name() << " creator.";
    registry_[key] = creator;
  }

  void Register(const SrcType& key, Creator creator, const string& help_msg) {
    Register(key, creator);
    help_message_[key] = help_msg;
  }

  inline bool Has(const SrcType& key) { return (registry_.count(key) != 0); }

  ObjectType* Create(const SrcType& key, Args ... args) {
    if (registry_.count(key) == 0) {
      // std::cerr << "Key " << key << " not found." << std::endl;
      // std::cerr << "Available keys:" << std::endl;
      // TODO: do we always want to print out the registered names? Sounds a bit
      // too verbose.
      //TEST_PrintRegisteredNames();
      // std::cerr << "Returning null pointer.";
      return nullptr;
    }
    return registry_[key](args...);
  }

  /**
   * Returns the keys currently registered as a vector.
   */
  vector<SrcType> Keys() {
    vector<SrcType> keys;
    for (const auto& it : registry_) {
      keys.push_back(it.first);
    }
    return keys;
  }

  const CaffeMap<SrcType, string>& HelpMessage() { return help_message_; }

 private:
  CaffeMap<SrcType, Creator> registry_;
  CaffeMap<SrcType, string> help_message_;
  std::mutex register_mutex_;

  DISABLE_COPY_AND_ASSIGN(Registry);
};

template <class SrcType, class ObjectType, class... Args>
class Registerer {
 public:
  Registerer(const SrcType& key,
             Registry<SrcType, ObjectType, Args...>* registry,
             typename Registry<SrcType, ObjectType, Args...>::Creator creator,
             const string& help_msg="") {
    registry->Register(key, creator, help_msg);
  }

  template <class DerivedType>
  static ObjectType* DefaultCreator(Args ... args) {
    return new DerivedType(args...);
  }
};

/**
 * CAFFE_ANONYMOUS_VARIABLE(str) introduces an identifier starting with
 * str and ending with a number that varies with the line.
 * Pretty much a copy from 'folly/Preprocessor.h'
 */
#define CAFFE_CONCATENATE_IMPL(s1, s2) s1##s2
#define CAFFE_CONCATENATE(s1, s2) CAFFE_CONCATENATE_IMPL(s1, s2)
#ifdef __COUNTER__
#define CAFFE_ANONYMOUS_VARIABLE(str) CAFFE_CONCATENATE(str, __COUNTER__)
#else
#define CAFFE_ANONYMOUS_VARIABLE(str) CAFFE_CONCATENATE(str, __LINE__)
#endif

#define CAFFE_DECLARE_TYPED_REGISTRY(RegistryName, SrcType, ObjectType, ...) \
  Registry<SrcType, ObjectType, ##__VA_ARGS__>* RegistryName();              \
  typedef Registerer<SrcType, ObjectType, ##__VA_ARGS__>                     \
      Registerer##RegistryName;

#define CAFFE_DEFINE_TYPED_REGISTRY(RegistryName, SrcType, ObjectType, ...) \
  Registry<SrcType, ObjectType, ##__VA_ARGS__>* RegistryName() {            \
    static Registry<SrcType, ObjectType, ##__VA_ARGS__>* registry =         \
        new Registry<SrcType, ObjectType, ##__VA_ARGS__>();                 \
    return registry;                                                        \
  }

// Note(Yangqing): The __VA_ARGS__ below allows one to specify a templated
// creator with comma in its templated arguments.
#define CAFFE_REGISTER_TYPED_CREATOR(RegistryName, key, ...)                  \
  namespace {                                                                 \
  static Registerer##RegistryName CAFFE_ANONYMOUS_VARIABLE(g_##RegistryName)( \
      key, RegistryName(), __VA_ARGS__);                                      \
  }

#define CAFFE_REGISTER_TYPED_CLASS(RegistryName, key, ...)                    \
  namespace {                                                                 \
  static Registerer##RegistryName CAFFE_ANONYMOUS_VARIABLE(g_##RegistryName)( \
      key,                                                                    \
      RegistryName(),                                                         \
      Registerer##RegistryName::DefaultCreator<__VA_ARGS__>);                 \
  }

// CAFFE_DECLARE_REGISTRY and CAFFE_DEFINE_REGISTRY are hard-wired to use string
// as the key
// type, because that is the most commonly used cases.
#define CAFFE_DECLARE_REGISTRY(RegistryName, ObjectType, ...) \
  CAFFE_DECLARE_TYPED_REGISTRY(                               \
      RegistryName, std::string, ObjectType, ##__VA_ARGS__)

#define CAFFE_DEFINE_REGISTRY(RegistryName, ObjectType, ...) \
  CAFFE_DEFINE_TYPED_REGISTRY(                               \
      RegistryName, std::string, ObjectType, ##__VA_ARGS__)

// CAFFE_REGISTER_CREATOR and CAFFE_REGISTER_CLASS are hard-wired to use string
// as the key
// type, because that is the most commonly used cases.
#define CAFFE_REGISTER_CREATOR(RegistryName, key, ...) \
  CAFFE_REGISTER_TYPED_CREATOR(RegistryName, #key, __VA_ARGS__)

#define CAFFE_REGISTER_CLASS(RegistryName, key, ...) \
  CAFFE_REGISTER_TYPED_CLASS(RegistryName, #key, __VA_ARGS__)

}  // namespace caffe2
#endif  // CAFFE2_CORE_REGISTRY_H_
