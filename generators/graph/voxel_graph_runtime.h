#ifndef VOXEL_GRAPH_RUNTIME_H
#define VOXEL_GRAPH_RUNTIME_H

#include "../../util/array_slice.h"
#include "../../util/math/interval.h"
#include "../../util/math/vector3i.h"
#include "program_graph.h"
#include <core/reference.h>

class ImageRangeGrid;

// CPU VM to execute a voxel graph generator
class VoxelGraphRuntime {
public:
	struct CompilationResult {
		bool success = false;
		int node_id = -1;
		String message;
	};

	struct Buffer {
		// Values of the buffer. Must contain at least `size` values.
		// TODO Consider wrapping this in debug mode. It is one of the rare cases I didnt do it.
		// I spent an hour debugging memory corruption which originated from an overrun while accessing this data.
		float *data = nullptr;
		// This size is not the allocated count, it's an available count below capacity.
		// All buffers have the same available count, size is here only for convenience.
		unsigned int size;
		unsigned int capacity;
		// Constant value of the buffer, if it is a compile-time constant
		float constant_value;
		// Is the buffer holding a compile-time constant
		bool is_constant;
		// Is the buffer a user input/output
		bool is_binding = false;
		// How many operations are using this buffer as input.
		// This value is only relevant when using optimized execution mapping.
		unsigned int local_users_count;
	};

	// Contains the data the program will modify while it runs.
	// The same state can be re-used with multiple programs, but it should be prepared before doing that.
	class State {
	public:
		inline const Buffer &get_buffer(uint16_t address) const {
			// TODO Just for convenience because STL bound checks aren't working in Godot 3
			CRASH_COND(address >= buffers.size());
			return buffers[address];
		}

		inline const Interval get_range(uint16_t address) const {
			// TODO Just for convenience because STL bound checks aren't working in Godot 3
			CRASH_COND(address >= buffers.size());
			return ranges[address];
		}

		void clear() {
			buffer_size = 0;
			buffer_capacity = 0;
			for (auto it = buffers.begin(); it != buffers.end(); ++it) {
				Buffer &b = *it;
				if (b.data != nullptr && !b.is_binding) {
					memfree(b.data);
				}
			}
			buffers.clear();
			ranges.clear();
		}

		ArraySlice<const int> get_debug_execution_map() const {
			return to_slice_const(debug_execution_map);
		}

	private:
		friend class VoxelGraphRuntime;

		std::vector<Interval> ranges;
		std::vector<Buffer> buffers;

		// Stores operation addresses
		std::vector<uint16_t> execution_map;

		// Stores node IDs referring to the user-facing graph
		std::vector<int> debug_execution_map;

		unsigned int execution_map_xzy_start_index;
		unsigned int buffer_size = 0;
		unsigned int buffer_capacity = 0;
	};

	VoxelGraphRuntime();
	~VoxelGraphRuntime();

	void clear();
	CompilationResult compile(const ProgramGraph &graph, bool debug);

	// Call this before you use a state with generation functions.
	// You need to call it once, until you want to use a different graph, buffer size or buffer count.
	// If none of these change, you can keep re-using it.
	void prepare_state(State &state, unsigned int buffer_size) const;

	float generate_single(State &state, Vector3 position, bool use_execution_map) const;

	void generate_set(State &state, ArraySlice<float> in_x, ArraySlice<float> in_y, ArraySlice<float> in_z,
			ArraySlice<float> out_sdf, bool skip_xz, bool use_execution_map) const;

	// Analyzes a specific region of inputs to find out what ranges of outputs we can expect.
	// It can be used to speed up calls to `generate_set` thanks to execution mapping,
	// so that operations can be optimized out if they don't contribute to the result.
	Interval analyze_range(State &state, Vector3i min_pos, Vector3i max_pos) const;

	// Call this after `analyze_range` if you intend to actually generate a set or single values in the area.
	// This allows to use the execution map optimization, until you choose another area.
	// (i.e when using this, querying values outside of the analyzed area may be invalid)
	inline void generate_optimized_execution_map(State &state, bool debug) const {
		generate_execution_map(state, state.execution_map, state.execution_map_xzy_start_index,
				debug ? &state.debug_execution_map : nullptr);
	}

	inline bool has_output() const {
		return _program.sdf_output_address != -1;
	}

	bool try_get_output_port_address(ProgramGraph::PortLocation port, uint16_t &out_address) const;

	struct HeapResource {
		void *ptr;
		void (*deleter)(void *p);
	};

	class CompileContext {
	public:
		CompileContext(const ProgramGraph::Node &node, std::vector<uint8_t> &program,
				std::vector<HeapResource> &heap_resources,
				std::vector<Variant> &params) :
				_node(node),
				_offset(program.size()),
				_program(program),
				_heap_resources(heap_resources),
				_params(params) {}

		Variant get_param(size_t i) const {
			CRASH_COND(i > _params.size());
			return _params[i];
		}

		// Typical use is to pass a struct containing all compile-time arguments the operation will need
		template <typename T>
		void set_params(T params) {
			// Can be called only once per node
			CRASH_COND(_offset != _program.size());
			_program.resize(_program.size() + sizeof(T));
			T &p = *reinterpret_cast<T *>(&_program[_offset]);
			p = params;
		}

		// In case the compilation step produces a resource to be deleted
		template <typename T>
		void add_memdelete_cleanup(T *ptr) {
			HeapResource hr;
			hr.ptr = ptr;
			hr.deleter = [](void *p) {
				// TODO We have no guarantee it was allocated with memnew :|
				T *tp = reinterpret_cast<T *>(p);
				memdelete(tp);
			};
			_heap_resources.push_back(hr);
		}

		void make_error(String message) {
			_error_message = message;
			_has_error = true;
		}

		bool has_error() const {
			return _has_error;
		}

		const String &get_error_message() const {
			return _error_message;
		}

	private:
		const ProgramGraph::Node &_node;
		const size_t _offset;
		std::vector<uint8_t> &_program;
		std::vector<HeapResource> &_heap_resources;
		std::vector<Variant> &_params;
		String _error_message;
		bool _has_error = false;
	};

	class _ProcessContext {
	public:
		inline _ProcessContext(
				const ArraySlice<const uint16_t> inputs,
				const ArraySlice<const uint16_t> outputs,
				const ArraySlice<const uint8_t> params) :
				_inputs(inputs),
				_outputs(outputs),
				_params(params) {}

		template <typename T>
		inline const T &get_params() const {
			return *reinterpret_cast<const T *>(_params.data());
		}

		inline uint32_t get_input_address(uint32_t i) const {
			return _inputs[i];
		}

	protected:
		inline uint32_t get_output_address(uint32_t i) const {
			return _outputs[i];
		}

	private:
		const ArraySlice<const uint16_t> _inputs;
		const ArraySlice<const uint16_t> _outputs;
		const ArraySlice<const uint8_t> _params;
	};

	class ProcessBufferContext : public _ProcessContext {
	public:
		inline ProcessBufferContext(
				const ArraySlice<const uint16_t> inputs,
				const ArraySlice<const uint16_t> outputs,
				const ArraySlice<const uint8_t> params,
				ArraySlice<Buffer> buffers) :
				_ProcessContext(inputs, outputs, params),
				_buffers(buffers) {}

		inline const Buffer &get_input(uint32_t i) const {
			const uint32_t address = get_input_address(i);
			return _buffers[address];
		}

		inline Buffer &get_output(uint32_t i) {
			const uint32_t address = get_output_address(i);
			return _buffers[address];
		}

	private:
		ArraySlice<Buffer> _buffers;
	};

	class RangeAnalysisContext : public _ProcessContext {
	public:
		inline RangeAnalysisContext(
				const ArraySlice<const uint16_t> inputs,
				const ArraySlice<const uint16_t> outputs,
				const ArraySlice<const uint8_t> params,
				ArraySlice<Interval> ranges,
				ArraySlice<Buffer> buffers) :
				_ProcessContext(inputs, outputs, params),
				_ranges(ranges),
				_buffers(buffers) {}

		inline const Interval get_input(uint32_t i) const {
			const uint32_t address = get_input_address(i);
			return _ranges[address];
		}

		inline void set_output(uint32_t i, const Interval r) {
			const uint32_t address = get_output_address(i);
			_ranges[address] = r;
		}

		inline void ignore_input(uint32_t i) {
			const uint32_t address = get_input_address(i);
			Buffer &b = _buffers[address];
			--b.local_users_count;
		}

	private:
		ArraySlice<Interval> _ranges;
		ArraySlice<Buffer> _buffers;
	};

	typedef void (*CompileFunc)(CompileContext &);
	typedef void (*ProcessBufferFunc)(ProcessBufferContext &);
	typedef void (*RangeAnalysisFunc)(RangeAnalysisContext &);

private:
	CompilationResult _compile(const ProgramGraph &graph, bool debug);

	void generate_execution_map(const State &state,
			std::vector<uint16_t> &execution_map, unsigned int &out_mapped_xzy_start,
			std::vector<int> *debug_execution_map) const;

	bool is_operation_constant(const State &state, uint16_t op_address) const;

	struct BufferSpec {
		// Index the buffer should be stored at
		uint16_t address;
		// How many nodes use this buffer as input
		uint16_t users_count;
		// Value of the compile-time constant, if any
		float constant_value;
		// Is the buffer constant at compile time
		bool is_constant;
		// Is the buffer a user input/output
		bool is_binding;
	};

	struct DependencyGraph {
		struct Node {
			uint16_t first_dependency;
			uint16_t end_dependency;
			uint16_t op_address;
			bool is_output;
			int debug_node_id;
		};

		// Indexes to the `nodes` array
		std::vector<uint16_t> dependencies;
		// Nodes in the same order they would be in the default execution map
		std::vector<Node> nodes;

		inline void clear() {
			dependencies.clear();
			nodes.clear();
		}
	};

	// Precalculated program data.
	// Remains constant and read-only after compilation.
	struct Program {
		// Serialized operations and arguments.
		// They come up as series of <opid><inputs><outputs><parameters_size><parameters>.
		// They should be laid out in the same order they will be run in, although it's not absolutely required.
		// It's better to have it ordered because memory access will be more predictable.
		std::vector<uint8_t> operations;

		// Describes dependencies between operations. It is generated at compile time.
		// It is used to perform dynamic optimization in case some operations can be predicted as constant.
		DependencyGraph dependency_graph;

		// List of indexes within `operations` describing which order they should be run into by default.
		// It's used because sometimes we may want to override with a simplified execution map dynamically.
		// When we don't, we use the default one so the code doesn't have to change.
		std::vector<uint16_t> default_execution_map;

		// Heap-allocated parameters data, when too large to fit in `operations`.
		// We keep a reference to them so they can be freed when the program is cleared.
		std::vector<HeapResource> heap_resources;

		// Heap-allocated parameters data, when too large to fit in `operations`.
		// We keep a reference to them so they won't be freed until the program is cleared.
		std::vector<Ref<Reference> > ref_resources;

		// Describes the list of buffers to prepare in `State` before the program can be run
		std::vector<BufferSpec> buffer_specs;

		// Address in `operations` from which operations will depend on Y. Operations before never depend on it.
		// It is used to optimize away calculations that would otherwise be the same in planar terrain use cases.
		uint32_t xzy_start_op_address;
		uint32_t xzy_start_execution_map_index;

		// Note: the following buffers are allocated by the user.
		// They are mapped temporarily into the same array of buffers inside `State`,
		// so we won't need specific code to handle them. This requires knowing at which index they are reserved.
		// They must be all assigned for the program to run correctly.
		//
		// Address within the State's array of buffers where the X input may be.
		int x_input_address = -1;
		// Address within the State's array of buffers where the Y input may be.
		int y_input_address = -1;
		// Address within the State's array of buffers where the Z input may be.
		int z_input_address = -1;
		// Address within the State's array of buffers where the SDF output may be.
		int sdf_output_address = -1;
		int sdf_output_node_index = -1;

		// Maximum amount of buffers this program will need to do a full run.
		// Buffers are needed to hold values of arguments and outputs for each operation.
		unsigned int buffer_count = 0;

		// Associates a high-level port to its corresponding address within the compiled program.
		// This is used for debugging intermediate values.
		HashMap<ProgramGraph::PortLocation, uint16_t, ProgramGraph::PortLocationHasher> output_port_addresses;

		// Result of the last compilation attempt. The program should not be run if it failed.
		CompilationResult compilation_result;

		void clear() {
			operations.clear();
			buffer_specs.clear();
			xzy_start_execution_map_index = 0;
			xzy_start_op_address = 0;
			default_execution_map.clear();
			output_port_addresses.clear();
			dependency_graph.clear();
			sdf_output_address = -1;
			x_input_address = -1;
			y_input_address = -1;
			z_input_address = -1;
			sdf_output_node_index = -1;
			compilation_result = CompilationResult();
			for (auto it = heap_resources.begin(); it != heap_resources.end(); ++it) {
				HeapResource &r = *it;
				CRASH_COND(r.deleter == nullptr);
				CRASH_COND(r.ptr == nullptr);
				r.deleter(r.ptr);
			}
			heap_resources.clear();
			unlock_images();
			ref_resources.clear();
			buffer_count = 0;
		}

		void lock_images();
		void unlock_images();
	};

	Program _program;
};

#endif // VOXEL_GRAPH_RUNTIME_H
