/*
 * Zeitlos SOC
 * Copyright (c) 2025 Lone Dynamics Corporation. All rights reserved.
 *
 * Address decode test for rtl/sysctl.v's nibble 0x7.
 *
 *   $ for a in "" -DICACHE; do for b in "" -DRTC; do \
 *         iverilog -o /tmp/t $a $b rtl/tb/tb_decode.v && /tmp/t; \
 *     done; done
 *
 * Nibble 0x7 has four tenants and two of them are optional (`ICACHE,
 * `RTC), so there are four builds to get right and only one of them is
 * the one anybody runs day to day. This walks every word in the nibble
 * and asserts EXACTLY ONE tenant claims each, in all four.
 *
 * Both failure directions matter and they fail differently:
 *
 *   claimed by nobody -- no ack on this bus, and picorv32_wb waits for
 *                        it forever. A dead machine, not a bad read.
 *   claimed by two    -- both drive the wbm_dat_i/wbm_ack muxes, so
 *                        the result depends on term order rather than
 *                        on anything meaningful.
 *
 * The decode expressions below are COPIED VERBATIM from sysctl.v
 * rather than instantiated, because sysctl.v cannot be elaborated
 * standalone (trailing commas in its port lists -- fine for yosys, not
 * for iverilog) and pulling in the whole SOC to test six wires would
 * be a poor trade. That copy is the weakness of this test: if
 * sysctl.v's decode changes, this must be updated to match or it will
 * keep passing while testing nothing.
 */
module tb_decode;
	reg [31:0] wbm_adr;

	wire cs_socctl = ((wbm_adr & 32'hf000_0300) == 32'h7000_0200);
`ifdef ICACHE
	wire cs_cache = ((wbm_adr & 32'hf000_0300) == 32'h7000_0100);
`endif
`ifdef RTC
	wire cs_rtc = ((wbm_adr & 32'hf000_0300) == 32'h7000_0300);
`endif
	wire cs_csrs = ((wbm_adr & 32'hf000_0000) == 32'h7000_0000)
		&& !cs_socctl
`ifdef ICACHE
		&& !cs_cache
`endif
`ifdef RTC
		&& !cs_rtc
`endif
		;

	integer i, n, errors;
	initial begin
		errors = 0;
		// walk every 4-byte word in the whole 0x7 nibble's low 4KB,
		// which covers all four sub-windows and their aliases
		for (i = 0; i < 4096; i = i + 4) begin
			wbm_adr = 32'h7000_0000 + i;
			#1;
			n = cs_csrs + cs_socctl;
`ifdef ICACHE
			n = n + cs_cache;
`endif
`ifdef RTC
			n = n + cs_rtc;
`endif
			if (n != 1) begin
				$display("FAIL: 0x%08x claimed by %0d tenants", wbm_adr, n);
				errors = errors + 1;
			end
		end
		// spot-check that each present tenant owns its documented base
		wbm_adr = 32'h7000_0000; #1;
		if (!cs_csrs) begin $display("FAIL: csrs does not own 0x7000_0000"); errors=errors+1; end
		wbm_adr = 32'h7000_0200; #1;
		if (!cs_socctl) begin $display("FAIL: socctl does not own 0x7000_0200"); errors=errors+1; end
		wbm_adr = 32'h7000_0100; #1;
`ifdef ICACHE
		if (!cs_cache) begin $display("FAIL: cache does not own 0x7000_0100"); errors=errors+1; end
`else
		if (!cs_csrs) begin $display("FAIL: csrs does not absorb 0x7000_0100"); errors=errors+1; end
`endif
		wbm_adr = 32'h7000_0300; #1;
`ifdef RTC
		if (!cs_rtc) begin $display("FAIL: rtc does not own 0x7000_0300"); errors=errors+1; end
`else
		if (!cs_csrs) begin $display("FAIL: csrs does not absorb 0x7000_0300"); errors=errors+1; end
`endif
		if (errors == 0) $display("PASS");
		else $display("FAIL (%0d)", errors);
		$finish;
	end
endmodule
