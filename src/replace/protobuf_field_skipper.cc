// google::protobuf::internal::FieldSkipper and CodedOutputStreamFieldSkipper.
// The decomp declares both but defines neither (its protobuf source is partial),
// and other classes derive from FieldSkipper, so its vtable is needed to link.
// Behaviour as in protobuf 2.6.0: forward to WireFormatLite.
#include "Contrib/Contrib/BlizzardProtoBuf/2.6.0/src/src/google/protobuf/wire_format_lite.h"

namespace google {
namespace protobuf {
namespace internal {

bool FieldSkipper::SkipField(io::CodedInputStream * input, uint32 tag)
{
    return WireFormatLite::SkipField(input, tag);
}

bool FieldSkipper::SkipMessage(io::CodedInputStream * input)
{
    return WireFormatLite::SkipMessage(input);
}

void FieldSkipper::SkipUnknownEnum(int, int)
{
}

CodedOutputStreamFieldSkipper::CodedOutputStreamFieldSkipper(io::CodedOutputStream * unknown_fields)
    : unknown_fields_(unknown_fields)
{
}

CodedOutputStreamFieldSkipper::~CodedOutputStreamFieldSkipper()
{
}

bool CodedOutputStreamFieldSkipper::SkipField(io::CodedInputStream * input, uint32 tag)
{
    return WireFormatLite::SkipField(input, tag, unknown_fields_);
}

bool CodedOutputStreamFieldSkipper::SkipMessage(io::CodedInputStream * input)
{
    return WireFormatLite::SkipMessage(input, unknown_fields_);
}

void CodedOutputStreamFieldSkipper::SkipUnknownEnum(int field_number, int value)
{
    unknown_fields_->WriteVarint32(field_number);
    unknown_fields_->WriteVarint64(value);
}

} // namespace internal
} // namespace protobuf
} // namespace google
