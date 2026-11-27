//! Offline compatibility check against the real Concordia lifter.
//!
//! This is deliberately not a VFG implementation. In particular, diagnostics
//! are fatal for executable eligibility: a PTX file containing comments in
//! place of a tensor instruction must never be used as an inference backend.
//! Build as a binary in tools/prepare_sass_cpu_tests.py's isolated workspace,
//! with serde_json as an additional dependency. Proprietary inputs/outputs
//! stay in the caller's evidence directory, outside the source tree.

use std::{collections::BTreeMap, env, fs, path::PathBuf};
use ptx::sass::{lift_sass_text_to_ptx, SassLiftOptions, TextDisassemblyParser};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let args: Vec<_> = env::args_os().collect();
    if args.len() != 3 {
        return Err("usage: vfg_sass_probe <disassembly-directory> <new-report-directory>".into());
    }
    let input = PathBuf::from(&args[1]).canonicalize()?;
    let output = PathBuf::from(&args[2]);
    fs::create_dir(&output)?;
    let mut files: Vec<_> = fs::read_dir(input)?
        .map(|e| e.map(|e| e.path()))
        .collect::<Result<_, _>>()?;
    files.retain(|p| p.extension().is_some_and(|e| e == "sass"));
    files.sort();
    if files.is_empty() { return Err("no disassemblies; no compatibility result".into()); }
    let mut modules = Vec::new();
    let mut totals = BTreeMap::<String, usize>::new();
    for file in files {
        let text = fs::read_to_string(&file)?;
        // The upstream parser is best-effort. It must not silently drop a
        // disassembled instruction and thereby make a module look portable.
        let expected_instructions = text.lines().filter(|line| {
            let Some(rest) = line.trim_start().strip_prefix("/*") else { return false; };
            let Some((address, rest)) = rest.split_once("*/") else { return false; };
            !address.is_empty() && address.chars().all(|c| c.is_ascii_hexdigit())
                && rest.contains(';') && !rest.trim_start().starts_with("/*")
        }).count();
        let instructions = TextDisassemblyParser::parse_cuobjdump_output(&text);
        if instructions.is_empty() { return Err(format!("no decoded instructions in {}", file.display()).into()); }
        let lifted = lift_sass_text_to_ptx(&text, SassLiftOptions {
            sm_version: 120,
            kernel_name: String::new(),
            include_sass_comments: false,
            emit_unsupported_comments: true,
        })?;
        let mut counts = BTreeMap::<String, usize>::new();
        let mut reasons = BTreeMap::<String, usize>::new();
        for d in &lifted.diagnostics {
            *counts.entry(d.opcode.clone()).or_default() += 1;
            *totals.entry(d.opcode.clone()).or_default() += 1;
            *reasons.entry(format!("{}: {}", d.opcode, d.message)).or_default() += 1;
        }
        let parsed = ptx_parser::parse_module_checked(&lifted.ptx);
        let parse_error = parsed.as_ref().err().map(|e| format!("{e:?}"));
        let eligible = expected_instructions == instructions.len()
            && lifted.diagnostics.is_empty() && parsed.is_ok();
        let stem = file.file_stem().ok_or("missing stem")?.to_string_lossy();
        // Keep incomplete text for debugging, never label it a loadable model.
        fs::write(output.join(format!("{stem}.diagnostic-only.ptx")), &lifted.ptx)?;
        let item = serde_json::json!({
            "file": file.file_name().unwrap().to_string_lossy(),
            "instructions": instructions.len(),
            "disassembledInstructions": expected_instructions,
            "allInstructionsDecoded": expected_instructions == instructions.len(),
            "diagnostics": lifted.diagnostics.len(),
            "unsupportedOpcodes": counts,
            "reasons": reasons,
            "ptxParsed": parsed.is_ok(),
            "parseError": parse_error,
            "translationEligibleForFurtherTesting": eligible,
            "gpuExecuted": false,
            "numericEquivalenceTested": false,
        });
        println!("module={stem} instructions={} diagnostics={} parsed={} eligible={eligible}",
            instructions.len(), lifted.diagnostics.len(), parsed.is_ok());
        modules.push(item);
    }
    let result = serde_json::json!({
        "status": "offline-diagnostic-not-an-AMD-VFG-runtime",
        "expectedConcordiaRevision": "cad55eb2d15fe1f4b1bccbf36dff6a0ae53ee1a1",
        "sourceIdentity": "Verify the external checkout and build receipt; not inferred from successful parsing.",
        "unsupportedOpcodes": totals,
        "modules": modules,
        "amdVfgImplemented": false,
        "amdInferenceTested": false,
    });
    fs::write(output.join("translation-report.json"), serde_json::to_vec_pretty(&result)?)?;
    Ok(())
}
