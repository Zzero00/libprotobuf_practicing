#include "mutator.h"
#include <fstream>
#include <iostream>
#include <unistd.h>

#include "google/protobuf/descriptor.h"
#include "google/protobuf/descriptor.pb.h"
#include "google/protobuf/reflection.h"

#include "libprotobuf-mutator/src/libfuzzer/libfuzzer_macro.h"

#include "gen/out.pb.h"
#include "libprotobuf-mutator/src/mutator.h"

#include <algorithm>
#include <random>
#include <cstdlib>


void ProtoToDataHelper(std::stringstream &out, const google::protobuf::Message &msg) {
  const google::protobuf::Descriptor *desc = msg.GetDescriptor();
  const google::protobuf::Reflection *refl = msg.GetReflection();

  const unsigned fields = desc->field_count();
  // std::cout << msg.DebugString() << std::endl;
  for (unsigned i = 0; i < fields; ++i) {
    const google::protobuf::FieldDescriptor *field = desc->field(i);

    // 只有当 Message 类型为 ChoiceList 和 Choice 时，该条件判断才能成立
    // 因为只有上述这两种类型的 field 才会是 Message 类型
    if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_MESSAGE) {
      // 如果此时的 field 类型就是 repeated Choice, 那么就说明当前 msg 是 ChoiceList 类型
      if (field->is_repeated()) {
        // 此时去 repeated Choice 类型的 field(可以把这个 field 理解成数组)中，遍历并取出每个 Choice 类型，之后将其输出
        const google::protobuf::RepeatedFieldRef<google::protobuf::Message> &ptr = refl->GetRepeatedFieldRef<google::protobuf::Message>(msg, field);
        // 将每个 choice 打出来
        for (const auto &child : ptr) {
          ProtoToDataHelper(out, child);
          out << "\n";
        }
      }
      /* 注意这里的 else，如果不是 ChoiceList 类型，那么此时就是 Choice 类型
         而对于 Choice 来说，其类型在 ××定义×× 时有五个 field
         但是，由于 oneof 的限制，实际代码运行的情况下，只会有其中一个 field
         对于 Choice 类型来说，能遍历到的 field 为：
         
         Message Choice {
            AllocChoice alloc_choice
            UpdateChoice update_choice
            DeleteChoice delete_choice
            ViewChoice view_choice
            ExitChoice exit_choice
         }

         但是当代码执行到这里时，msg 参数中的 field 就只会有一个，例如 
         msg {
             DeleteChoice
         }
         也就是说，从 msg.desc 中取出来的 field（即用于遍历的 Field), 是会大于 msg 中实际保存的 field
         因此需要额外加一个 HasField 判断，用于说明当前 msg（Choice类型） 里存放的确实是 DeleteChoice
         这样就可以在下面的 if 分支内部，单独将这个 DeleteChoice 取出来，并进行操作
      */
      else if (refl->HasField(msg, field)) {
        // 获取到某一个 choice 的 message
        const google::protobuf::Message &child = refl->GetMessage(msg, field);
        
        // 输出其 choice ID
        std::string choice_typename = child.GetDescriptor()->name();
        if(choice_typename == "AllocChoice")
            out << "1 ";
        else if(choice_typename == "UpdateChoice")
            out << "2 ";
        else if(choice_typename == "DeleteChoice")
            out << "3 ";
        else if(choice_typename == "ViewChoice")
            out << "4 ";
        else if(choice_typename == "ExitChoice")
            out << "5 ";
        else
            abort();
        
        // 输出剩余的 field
        ProtoToDataHelper(out, child);
      }
    } 
    // 对于单个 field
    else if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_INT32) {
      out << refl->GetInt32(msg, field);
      if(i < fields - 1) 
        out << " ";
    } 
    else if (field->cpp_type() == google::protobuf::FieldDescriptor::CPPTYPE_STRING) {
      out << refl->GetString(msg, field);
      if(i < fields - 1) 
        out << " ";
    } 
    else {
      abort();
    }

  }
}

// Apparently the textual generator kinda breaks?
DEFINE_BINARY_PROTO_FUZZER(const menuctf::ChoiceList &root) {
  std::stringstream stream;
  ProtoToDataHelper(stream, root);
  std::string data = stream.str();
  std::replace(data.begin(), data.end(), '\n', '|');
  puts(("ProtoToDataHelper: " + data).c_str());
  puts("====================================================================================================");
  // std::string dbg = root.DebugString();
  // std::replace(dbg.begin(), dbg.end(), '\n', ' ');
}

// AFLPlusPlus interface
extern "C" {
  void *afl_custom_init(void *afl, unsigned int seed) {
    #pragma unused (afl)

    auto mutator = new protobuf_mutator::Mutator();
    mutator->Seed(seed);
    return mutator;
  }
  
  void afl_custom_deinit(void *data) {
    protobuf_mutator::Mutator *mutator = (protobuf_mutator::Mutator*)data;
    delete mutator;
  }
  
  // afl_custom_fuzz
  size_t afl_custom_fuzz(void *data, unsigned char *buf, size_t buf_size, unsigned char **out_buf, 
                         unsigned char *add_buf, size_t add_buf_size, size_t max_size) {
    #pragma unused (add_buf)
    #pragma unused (add_buf_size)
    
    static uint8_t *saved_buf = nullptr;

    assert(buf_size <= max_size);
    
    uint8_t *new_buf = (uint8_t *) realloc((void *)saved_buf, max_size);
    if (!new_buf) 
        abort();
    saved_buf = new_buf;

    protobuf_mutator::Mutator *mutator = (protobuf_mutator::Mutator*)data;

    menuctf::ChoiceList msg;
    if (!protobuf_mutator::libfuzzer::LoadProtoInput(true, buf, buf_size, &msg))
      abort();

    // 弃用合并两个 Message 的 CrossOver 函数，因为它的变异效果有亿点点拉胯
    mutator->Mutate(&msg, max_size);
    
    std::string out_str;
    msg.SerializePartialToString(&out_str);

    memcpy(new_buf, out_str.c_str(), out_str.size());
    *out_buf = new_buf;
    return out_str.size();
  }

  size_t afl_custom_post_process(void* data, uint8_t *buf, size_t buf_size, uint8_t **out_buf) {
    #pragma unused (data)
    // new_data is never free'd by pre_save_handler
    // I prefer a slow but clearer implementation for now
    
    static uint8_t *saved_buf = NULL;

    menuctf::ChoiceList msg;
    std::stringstream stream;
    // 如果加载成功
    if (protobuf_mutator::libfuzzer::LoadProtoInput(true, buf, buf_size, &msg))
      ProtoToDataHelper(stream, msg);
    else
      // 必须保证成功
      abort();
    const std::string str = stream.str();

    uint8_t *new_buf = (uint8_t *) realloc((void *)saved_buf, str.size());
    if (!new_buf) {
      *out_buf = buf;
      return buf_size;
    }
    *out_buf = saved_buf = new_buf;

    memcpy((void *)new_buf, str.c_str(), str.size());

    return str.size();
  }
}