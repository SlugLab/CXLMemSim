// Damer Qwen27B Hardware-JIT policy bundle for CXLMemSim CXL switch.
//
// This module is the generated RTL-side policy table that the switch small core
// instantiates next to the data-movement engines. The C++ switch runtime uses
// include/damer_hwjit_policy.h as the behavioral twin for qtest execution.
module damer_cxl_switch_hwjit_qwen27b_policy (
  input  logic        clk,
  input  logic        rst_n,
  input  logic        event_valid,
  output logic        event_ready,
  input  logic        cmd_ready,
  input  logic [31:0] switchlet_id,
  input  logic [63:0] event_bytes,
  input  logic [31:0] event_tiles,
  output logic        cmd_valid,
  output logic        observe_valid,
  output logic        policy_ok,
  output logic [31:0] cmd_edge_index,
  output logic [31:0] cmd_source_node,
  output logic [31:0] cmd_destination_node,
  output logic [31:0] cmd_placement_node,
  output logic [31:0] cmd_transform_mask,
  output logic [31:0] cmd_ordering,
  output logic [31:0] cmd_ownership,
  output logic [31:0] cmd_alias_set,
  output logic [63:0] cmd_bytes,
  output logic [63:0] cmd_stride,
  output logic [63:0] cmd_reuse_distance,
  output logic [31:0] cmd_read_ratio,
  output logic [31:0] cmd_write_ratio,
  output logic [31:0] cmd_priority,
  output logic [31:0] cmd_ttl,
  output logic [31:0] cmd_max_ops
);

  localparam int SOURCE_CXL_MEMORY = 2;
  localparam int DEST_CXL_MEMORY = 2;
  localparam int PLACEMENT_SWITCH_COMPUTE = 5;

  typedef enum logic [1:0] {
    STATE_IDLE = 2'd0,
    STATE_EMIT = 2'd1,
    STATE_DONE = 2'd2
  } state_t;

  state_t state;

  assign event_ready = (state == STATE_IDLE);
  assign cmd_valid = (state == STATE_EMIT) && policy_ok;
  assign observe_valid = cmd_valid;
  assign cmd_edge_index = 32'd0;

  always_ff @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      state <= STATE_IDLE;
    end else begin
      unique case (state)
        STATE_IDLE: begin
          if (event_valid && policy_ok)
            state <= STATE_EMIT;
        end
        STATE_EMIT: begin
          if (cmd_ready)
            state <= STATE_DONE;
        end
        default: begin
          state <= STATE_IDLE;
        end
      endcase
    end
  end

  always_comb begin
    policy_ok = 1'b1;
    cmd_source_node = 32'd2;
    cmd_destination_node = 32'd2;
    cmd_placement_node = 32'd5;
    cmd_transform_mask = 32'd0;
    cmd_ordering = 32'd1;
    cmd_ownership = 32'd3;
    cmd_alias_set = 32'h7eb5431a ^ switchlet_id;
    cmd_bytes = event_bytes;
    cmd_stride = 64'd1;
    cmd_reuse_distance = 64'd0;
    cmd_read_ratio = 32'd1;
    cmd_write_ratio = 32'd1;
    cmd_priority = 32'd8;
    cmd_ttl = 32'd4;
    cmd_max_ops = 32'd1;

    unique case (switchlet_id)
      32'd0: cmd_transform_mask = 32'd0;   // qwen27b_logits_move
      32'd1: cmd_transform_mask = 32'd1;   // qwen27b_kv_pack
      32'd2: cmd_transform_mask = 32'd2;   // qwen27b_prefill_activation_spill
      32'd3: cmd_transform_mask = 32'd4;   // qwen27b_decode_kv_fetch
      32'd4: cmd_transform_mask = 32'd8;   // qwen27b_attention_mask_filter
      32'd5: cmd_transform_mask = 32'd16;  // qwen27b_tp_logits_reduce
      32'd6: cmd_transform_mask = 32'd32;  // qwen27b_tensor_shard_exchange
      32'd7: cmd_transform_mask = 32'd64;  // qwen27b_kv_replica_refresh
      32'd8: cmd_transform_mask = 32'd128; // qwen27b_checkpoint_persist
      default: begin
        policy_ok = 1'b0;
        cmd_transform_mask = 32'd0;
      end
    endcase

    if (event_tiles == 32'd0) begin
      policy_ok = 1'b0;
    end
  end

  initial begin
    if (SOURCE_CXL_MEMORY < 0) $fatal("invalid source node");
    if (PLACEMENT_SWITCH_COMPUTE < 0) $fatal("invalid placement node");
  end
endmodule
