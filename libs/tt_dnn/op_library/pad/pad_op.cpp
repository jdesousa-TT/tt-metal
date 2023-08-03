#include "tt_dnn/op_library/pad/pad_op.hpp"
#include "tt_dnn/op_library/math.hpp"

#include "tt_metal/host_api.hpp"
#include "tt_metal/common/constants.hpp"
#include "tt_metal/detail/util.hpp"

using namespace tt::constants;

namespace tt {

namespace tt_metal {

operation::ProgramWithCallbacks pad_rm(const Tensor &a, Tensor &output, const Shape &output_tensor_shape, const Shape &input_tensor_start, const float pad_value) {

    tt_metal::Program program{};

    CoreRange core = {.start={0, 0}, .end={0, 0}};

    // This should allocate a DRAM buffer on the device
    tt_metal::Device *device = a.device();

    auto output_shape = output_tensor_shape;

    tt_metal::Buffer *src0_buffer = a.buffer();

    uint32_t unpadded_row_size_bytes = a.shape()[3] * a.element_size();
    uint32_t padded_row_size_bytes = output_shape[3] * a.element_size();

    tt_metal::Buffer *dst_buffer = output.buffer();
    TT_ASSERT(dst_buffer != nullptr, "Output buffer should be allocated on device!");

    uint32_t src_stick_size = unpadded_row_size_bytes;
    uint32_t dst_stick_size = padded_row_size_bytes;

    uint32_t dst_buffer_size = dst_stick_size;

    auto dst_buffer_l1 = tt_metal::Buffer(device, dst_buffer_size, dst_buffer_size, tt_metal::BufferType::L1);

    bfloat16 bfloat_pad_value = bfloat16(pad_value);
    uint32_t packed_pad_value = pack_two_bfloat16_into_uint32({bfloat_pad_value, bfloat_pad_value});

    vector<uint32_t> reader_kernel_args = {
        src0_buffer->address(),
        dst_buffer->address(),
        a.shape()[0],
        output_shape[0],
        a.shape()[1],
        output_shape[1],
        a.shape()[2],
        output_shape[2],
        a.shape()[3],
        output_shape[3],
        unpadded_row_size_bytes,
        padded_row_size_bytes,
        padded_row_size_bytes - unpadded_row_size_bytes,
        packed_pad_value,
        dst_buffer_l1.address()
    };
    bool src0_is_dram = src0_buffer->buffer_type() == tt_metal::BufferType::DRAM ? 1 : 0;
    bool dst_is_dram = dst_buffer->buffer_type() == tt_metal::BufferType::DRAM ? 1 : 0;
    bool src_stick_size_is_power_of_two = is_power_of_two_at_least_32(src_stick_size);
    uint32_t src_log2_stick_size = src_stick_size_is_power_of_two ? (std::uint32_t)log2(src_stick_size) : 0;
    bool dst_stick_size_is_power_of_two = is_power_of_two_at_least_32(dst_stick_size);
    uint32_t dst_log2_stick_size = dst_stick_size_is_power_of_two ? (std::uint32_t)log2(dst_stick_size) : 0;
    std::vector<uint32_t> compile_time_args_vec = {
        (std::uint32_t) src0_is_dram,
        (std::uint32_t) dst_is_dram,
        (std::uint32_t) src_stick_size_is_power_of_two,
        (std::uint32_t) src_log2_stick_size,
        (std::uint32_t) dst_stick_size_is_power_of_two,
        (std::uint32_t) dst_log2_stick_size,

    };

    // Tilized reader
    tt_metal::KernelID unary_reader_kernel_id = tt_metal::CreateDataMovementKernel(
        program,
        "tt_metal/kernels/dataflow/pad_dims_rm_interleaved.cpp",
        core,
        tt_metal::DataMovementConfig{.processor = tt_metal::DataMovementProcessor::RISCV_1, .noc = tt_metal::NOC::RISCV_1_default, .compile_args = compile_time_args_vec});

    tt_metal::SetRuntimeArgs(
        program,
        unary_reader_kernel_id,
        core,
        reader_kernel_args
    );

    auto override_runtime_args_callback = [kernel_id=unary_reader_kernel_id](
        const Program &program,
        const std::vector<Buffer*>& input_buffers,
        const std::vector<Buffer*>& output_buffers
    ) {

        auto src_buffer = input_buffers.at(0);
        auto dst_buffer = output_buffers.at(0);

        CoreCoord core = {0, 0};

        {
            auto runtime_args = GetRuntimeArgs(program, kernel_id, core);
            runtime_args[0] = src_buffer->address();
            runtime_args[1] = dst_buffer->address();
            SetRuntimeArgs(program, kernel_id, core, runtime_args);
        }
    };

    return {std::move(program), override_runtime_args_callback};
}

operation::ProgramWithCallbacks pad_tile(const Tensor &a, Tensor& output, const Shape &output_tensor_shape, const Shape &input_tensor_start, const float pad_value) {

    tt_metal::Program program{};

    CoreRange core = {.start={0, 0}, .end={0, 0}};

    // This should allocate a DRAM buffer on the device
    tt_metal::Device *device = a.device();

    auto output_shape = output_tensor_shape;

    tt_metal::Buffer *src0_buffer = a.buffer();

    tt_metal::Buffer *dst_buffer = output.buffer();
    TT_ASSERT(dst_buffer != nullptr, "Output buffer should be allocated on device!");

    tt::DataFormat cb_data_format = tt_metal::datatype_to_dataformat_converter(a.dtype());
    uint32_t single_tile_size = tt_metal::detail::TileSize(cb_data_format);

    uint32_t src0_cb_index = 0;
    uint32_t num_input_tiles = 2;

    auto cb_src0 = tt_metal::CreateCircularBuffers(
        program,
        src0_cb_index,
        core,
        num_input_tiles,
        num_input_tiles * single_tile_size,
        cb_data_format
    );

    uint32_t src1_cb_index = 1; // For pad buffer
    uint32_t num_pad_tiles = 1;
    auto cb_src1 = tt_metal::CreateCircularBuffers(
        program,
        src1_cb_index,
        core,
        num_pad_tiles,
        num_pad_tiles * single_tile_size,
        cb_data_format
    );

    bfloat16 bfloat_pad_value = bfloat16(pad_value);
    uint32_t packed_pad_value = pack_two_bfloat16_into_uint32({bfloat_pad_value, bfloat_pad_value});

    uint32_t num_unpadded_Xt = a.shape()[3] / TILE_WIDTH;
    uint32_t num_total_Xt = output_shape[3] / TILE_WIDTH;
    uint32_t num_padded_Xt = num_total_Xt - num_unpadded_Xt;
    uint32_t num_unpadded_Yt = a.shape()[2] / TILE_HEIGHT;
    uint32_t num_total_Yt = output_shape[2] / TILE_HEIGHT;
    uint32_t num_padded_Yt = (num_total_Yt - num_unpadded_Yt) * num_total_Xt;
    uint32_t num_unpadded_Z = a.shape()[1];
    uint32_t num_total_Z = output_shape[1];
    uint32_t num_padded_Zt = (num_total_Z - num_unpadded_Z) * num_total_Yt * num_total_Xt;
    uint32_t num_unpadded_W = a.shape()[0];
    uint32_t num_total_W = output_shape[0];
    uint32_t num_padded_Wt = (num_total_W - num_unpadded_W) * num_total_Z * num_total_Yt * num_total_Xt;

    uint32_t num_unpadded_tiles = a.volume() / TILE_HW;

    vector<uint32_t> reader_kernel_args = {
        src0_buffer->address(),
        num_unpadded_tiles, 0
    };
    vector<uint32_t> writer_kernel_args = {
        dst_buffer->address(),
        num_unpadded_W,
        num_padded_Wt,
        num_unpadded_Z,
        num_padded_Zt,
        num_unpadded_Yt,
        num_padded_Yt,
        num_unpadded_Xt,
        num_padded_Xt,
        packed_pad_value,
    };

    // Reader compile-time args
    // Data is 32 byte aligned
    bool src0_is_dram = src0_buffer->buffer_type() == tt_metal::BufferType::DRAM ? 1 : 0;
    bool dst_is_dram = dst_buffer->buffer_type() == tt_metal::BufferType::DRAM ? 1 : 0;
    std::vector<uint32_t> reader_compile_time_args = {
        // interleaved accessor args
        (std::uint32_t) src0_is_dram
    };
    std::vector<uint32_t> writer_compile_time_args = {
        // interleaved accessor args
        (std::uint32_t) src0_cb_index,
        (std::uint32_t) src1_cb_index,
        (std::uint32_t) dst_is_dram
    };
    // Tilized reader
    tt_metal::KernelID unary_reader_kernel_id = tt_metal::CreateDataMovementKernel(
        program,
        "tt_metal/kernels/dataflow/reader_unary_interleaved_start_id.cpp",
        core,
        tt_metal::DataMovementConfig{.processor = tt_metal::DataMovementProcessor::RISCV_1, .noc = tt_metal::NOC::RISCV_1_default, .compile_args = reader_compile_time_args});

    tt_metal::KernelID unary_writer_kernel_id = tt_metal::CreateDataMovementKernel(
        program,
        "tt_metal/kernels/dataflow/writer_unary_pad_dims_interleaved.cpp",
        core,
        tt_metal::DataMovementConfig{.processor = tt_metal::DataMovementProcessor::RISCV_0, .noc = tt_metal::NOC::RISCV_0_default, .compile_args = writer_compile_time_args});

    tt_metal::SetRuntimeArgs(
        program,
        unary_reader_kernel_id,
        core,
        reader_kernel_args
    );

    tt_metal::SetRuntimeArgs(
        program,
        unary_writer_kernel_id,
        core,
        writer_kernel_args
    );

    auto override_runtime_args_callback = [unary_reader_kernel_id, unary_writer_kernel_id](
        const Program &program,
        const std::vector<Buffer*>& input_buffers,
        const std::vector<Buffer*>& output_buffers
    ) {

        auto src_dram_buffer = input_buffers.at(0);

        auto dst_dram_buffer = output_buffers.at(0);

        CoreCoord core = {0, 0};

        {
            auto runtime_args = GetRuntimeArgs(program, unary_reader_kernel_id, core);
            runtime_args[0] = src_dram_buffer->address();
            SetRuntimeArgs(program, unary_reader_kernel_id, core, runtime_args);
        }

        {
            auto runtime_args = GetRuntimeArgs(program, unary_writer_kernel_id, core);
            runtime_args[0] = dst_dram_buffer->address();
            SetRuntimeArgs(program, unary_writer_kernel_id, core, runtime_args);
        }
    };

    return {std::move(program), override_runtime_args_callback};
}


void Pad::validate(const std::vector<Tensor> &input_tensors) const {
    const auto& input_tensor = input_tensors.at(0);
    TT_ASSERT(input_tensor.storage_type() == StorageType::DEVICE, "Operand to pad needs to be on device!");
    TT_ASSERT(input_tensor.buffer() != nullptr, "Operand to pad needs to be allocated in a buffer on device!");
    TT_ASSERT(input_tensor.layout() == Layout::TILE || input_tensor.layout() == Layout::ROW_MAJOR);
    TT_ASSERT(
        (this->input_tensor_start[0] == 0 && this->input_tensor_start[1] == 0 && this->input_tensor_start[2] == 0 && this->input_tensor_start[3] == 0),
        "On device padding only supports padding at end of dims"
    );
    TT_ASSERT(input_tensor.shape()[0] + this->input_tensor_start[0] <= this->output_tensor_shape[0], "Output size cannot fit input with offset");
    TT_ASSERT(input_tensor.shape()[1] + this->input_tensor_start[1] <= this->output_tensor_shape[1], "Output size cannot fit input with offset");
    TT_ASSERT(input_tensor.shape()[2] + this->input_tensor_start[2] <= this->output_tensor_shape[2], "Output size cannot fit input with offset");
    TT_ASSERT(input_tensor.shape()[3] + this->input_tensor_start[3] <= this->output_tensor_shape[3], "Output size cannot fit input with offset");

    if (input_tensor.layout() == Layout::TILE) {
        TT_ASSERT((this->output_tensor_shape[2] % TILE_HEIGHT == 0), "Can only pad tilized tensor with full tiles");
        TT_ASSERT((this->output_tensor_shape[3] % TILE_WIDTH == 0), "Can only pad tilized tensor with full tiles");
        TT_ASSERT(input_tensor.dtype() == DataType::BFLOAT16, "Cannot pad tilized tensor with specified format");
    } else if (input_tensor.layout() == Layout::ROW_MAJOR) {
        TT_ASSERT(this->output_tensor_shape[3] % 2 == 0, "RM padding requires output X dim to be a multiple of 2");
        TT_ASSERT(input_tensor.dtype() == DataType::BFLOAT16, "Cannot pad RM tensor with specified format");
    }
}

std::vector<Shape> Pad::compute_output_shapes(const std::vector<Tensor> &input_tensors) const {
    auto input_shape = input_tensors.at(0).shape();
    auto dimensions_pads = std::vector<Padding::PadDimension>();
    for (auto index = 0; index < input_shape.rank(); index++) {
        auto front = this->input_tensor_start[index];
        auto back = this->output_tensor_shape[index] - (this->input_tensor_start[index] + input_shape[index]);
        dimensions_pads.push_back(Padding::PadDimension{.front=front, .back=back});
    }
    const auto padding = Padding(dimensions_pads, Padding::PadValue::Any);
    return {Shape(this->output_tensor_shape, padding)};
}

std::vector<Tensor> Pad::create_output_tensors(const std::vector<Tensor>& input_tensors) const {
    const auto& input_tensor = input_tensors.at(0);
    return operation::generic_create_output_tensors(*this, input_tensors, input_tensor.dtype(), input_tensor.layout(), this->output_mem_config);
}

// TODO: If pad is called on a tile and output is not tile, we could untilize then pad, and output is RM
// Currently calling pad on a tile requires the output pad shape to be tile
operation::ProgramWithCallbacks Pad::create_program(const std::vector<Tensor>& input_tensors, std::vector<Tensor> &output_tensors) const {
    const auto& input_tensor = input_tensors.at(0);
    auto& output_tensor = output_tensors.at(0);
    if (input_tensor.layout() == Layout::ROW_MAJOR) {
        return pad_rm(input_tensor, output_tensor, this->output_tensor_shape, this->input_tensor_start, this->pad_value);
    } else if (input_tensor.layout() == Layout::TILE) {
        return pad_tile(input_tensor, output_tensor, this->output_tensor_shape, this->input_tensor_start, this->pad_value);
    } else {
        TT_ASSERT(false, "Unsupported layout for pad");
        return {};
    }
}

operation::Hash Pad::compute_program_hash(const std::vector<Tensor> &input_tensors) const {
    const auto& input_tensor = input_tensors.at(0);
    return fmt::format("{}_{}", *this, input_tensor);
}

tt::stl::reflection::Attributes Pad::attributes() const {
    return {
        {"output_tensor_shape", this->output_tensor_shape},
        {"input_tensor_start", this->input_tensor_start},
        {"pad_value", this->pad_value},
        {"output_mem_config", this->output_mem_config},
    };
}

Tensor pad(const Tensor &input_tensor, const Shape &output_tensor_shape, const Shape &input_tensor_start, float pad_value, const MemoryConfig& mem_config) {
    if (input_tensor.shape() == output_tensor_shape) {
        return input_tensor;
    }
    return operation::run_without_autoformat(Pad{output_tensor_shape, input_tensor_start, pad_value, mem_config}, {input_tensor}).at(0);

}

void PadOnHost::validate(const std::vector<Tensor> &input_tensors) const {
    const auto& input_tensor = input_tensors.at(0);
    TT_ASSERT(input_tensor.storage_type() == StorageType::OWNED or input_tensor.storage_type() == StorageType::BORROWED);
    TT_ASSERT(input_tensor.layout() == Layout::ROW_MAJOR);
    TT_ASSERT(input_tensor.shape()[0] + this->input_tensor_start[0] <= this->output_tensor_shape[0], "Output size cannot fit input with offset");
    TT_ASSERT(input_tensor.shape()[1] + this->input_tensor_start[1] <= this->output_tensor_shape[1], "Output size cannot fit input with offset");
    TT_ASSERT(input_tensor.shape()[2] + this->input_tensor_start[2] <= this->output_tensor_shape[2], "Output size cannot fit input with offset");
    TT_ASSERT(input_tensor.shape()[3] + this->input_tensor_start[3] <= this->output_tensor_shape[3], "Output size cannot fit input with offset");
}

std::vector<Shape> PadOnHost::compute_output_shapes(const std::vector<Tensor> &input_tensors) const {
    auto input_shape = input_tensors.at(0).shape();
    auto dimensions_pads = std::vector<Padding::PadDimension>();
    for (auto index = 0; index < input_shape.rank(); index++) {
        auto front = this->input_tensor_start[index];
        auto back = this->output_tensor_shape[index] - (this->input_tensor_start[index] + input_shape[index]);
        dimensions_pads.push_back(Padding::PadDimension{.front=front, .back=back});
    }
    const auto padding = Padding(dimensions_pads, Padding::PadValue::Any);
    return {Shape(this->output_tensor_shape, padding)};
}

std::vector<Tensor> PadOnHost::compute_output_tensors(const std::vector<Tensor>& input_tensors) const {
    auto output_shape = this->compute_output_shapes(input_tensors).at(0);
    const auto& input_tensor = input_tensors.at(0);
    if (input_tensor.shape() == this->output_tensor_shape) {
        return {input_tensor};
    } else {
        return {input_tensor.pad(output_shape, this->input_tensor_start, this->pad_value)};
    }
}

tt::stl::reflection::Attributes PadOnHost::attributes() const {
    return {
        {"output_tensor_shape", this->output_tensor_shape},
        {"input_tensor_start", this->input_tensor_start},
        {"pad_value", this->pad_value},
    };
}

Tensor pad_on_host(const Tensor &input_tensor, const Shape &output_tensor_shape, const Shape &input_tensor_start, float pad_value) {
    return operation::run(PadOnHost{output_tensor_shape, input_tensor_start, pad_value}, {input_tensor}).at(0);
}

}  // namespace tt_metal

}  // namespace tt
