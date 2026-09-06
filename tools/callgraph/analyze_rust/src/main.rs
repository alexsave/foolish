// Rust analyzer: real parsing via `syn` (full AST), not regex.
//
// Every fn / impl-method / trait-default-method becomes a node; every call
// expression, method call and macro invocation inside a body becomes an edge.
// Callee resolution is by name (syn has no type inference), so confidence is
// recorded per edge exactly like the C/Swift/TS analyzers do.
use serde_json::json;
use std::collections::{BTreeMap, BTreeSet};
use syn::spanned::Spanned;
use syn::visit::Visit;

struct Calls {
    out: Vec<(String, usize, &'static str)>, // (callee name, line, style)
    // Line ranges of closures that got their own node; their calls belong to
    // them, not to the function they are declared in.
    skip: Vec<(usize, usize)>,
}

fn spans(b: &syn::Expr) -> (usize, usize) {
    (b.span().start().line, b.span().end().line)
}

/// `let name = |args| body;` - a named closure. TypeScript arrow functions get
/// their own nodes, so Rust closures do too; without this a body like lib.rs's
/// `rd32` reads as an unresolved callee when it is right there in the file.
struct Closures {
    out: Vec<(String, usize, syn::Expr)>,
}
impl<'ast> Visit<'ast> for Closures {
    fn visit_local(&mut self, node: &'ast syn::Local) {
        if let (syn::Pat::Ident(id), Some(init)) = (&node.pat, &node.init) {
            if let syn::Expr::Closure(c) = &*init.expr {
                self.out.push((
                    id.ident.to_string(),
                    id.ident.span().start().line,
                    (*c.body).clone(),
                ));
            }
        }
        syn::visit::visit_local(self, node);
    }
}

impl<'ast> Visit<'ast> for Calls {
    fn visit_expr_closure(&mut self, node: &'ast syn::ExprClosure) {
        let (a, b) = spans(&node.body);
        if self.skip.iter().any(|&(x, y)| x <= a && b <= y) {
            return;
        }
        syn::visit::visit_expr_closure(self, node);
    }
    fn visit_expr_call(&mut self, node: &'ast syn::ExprCall) {
        if let syn::Expr::Path(p) = &*node.func {
            if let Some(seg) = p.path.segments.last() {
                let line = seg.ident.span().start().line;
                // Qualified paths keep their last-but-one segment as a hint
                // (`Type::new`), bare paths are just the fn name.
                let name = if p.path.segments.len() >= 2 {
                    let n = p.path.segments.len();
                    format!("{}::{}", p.path.segments[n - 2].ident, seg.ident)
                } else {
                    seg.ident.to_string()
                };
                // `Some(x)`, `Card(..)`, `Lcg(..)`: a tuple-struct or enum
                // variant constructor, not a call into a body.
                let bare = !name.contains("::");
                let cap = seg.ident.to_string().chars().next().is_some_and(|c| c.is_uppercase());
                if !(bare && cap) {
                    self.out.push((name, line, "call"));
                }
            }
        }
        syn::visit::visit_expr_call(self, node);
    }
    fn visit_expr_method_call(&mut self, node: &'ast syn::ExprMethodCall) {
        self.out.push((
            node.method.to_string(),
            node.method.span().start().line,
            "method",
        ));
        syn::visit::visit_expr_method_call(self, node);
    }
    fn visit_macro(&mut self, node: &'ast syn::Macro) {
        if let Some(seg) = node.path.segments.last() {
            self.out.push((
                format!("{}!", seg.ident),
                seg.ident.span().start().line,
                "macro",
            ));
        }
    }
}

struct Def {
    id: String,
    name: String,
    short: String,
    file: String,
    line: usize,
    kind: &'static str,
    exported: bool,
    loc: usize,
    module: String,
    calls: Vec<(String, usize, &'static str)>,
}

fn type_name(t: &syn::Type) -> String {
    match t {
        syn::Type::Path(p) => p
            .path
            .segments
            .last()
            .map(|s| s.ident.to_string())
            .unwrap_or_else(|| "?".into()),
        syn::Type::Reference(r) => type_name(&r.elem),
        _ => "?".into(),
    }
}

fn named_closures(block: &syn::Block) -> Vec<(String, usize, syn::Expr)> {
    let mut c = Closures { out: Vec::new() };
    c.visit_block(block);
    c.out
}

fn body_calls(block: &syn::Block) -> Vec<(String, usize, &'static str)> {
    let skip = named_closures(block).iter().map(|(_, _, b)| spans(b)).collect();
    let mut c = Calls { out: Vec::new(), skip };
    c.visit_block(block);
    c.out
}

fn closure_calls(body: &syn::Expr) -> Vec<(String, usize, &'static str)> {
    let mut c = Calls { out: Vec::new(), skip: Vec::new() };
    c.visit_expr(body);
    c.out
}

fn span_loc(block: &syn::Block) -> usize {
    let a = block.brace_token.span.open().start().line;
    let b = block.brace_token.span.close().end().line;
    b.saturating_sub(a) + 1
}

fn push_fn(
    defs: &mut Vec<Def>,
    file: &str,
    module: &str,
    prefix: Option<&str>,
    sig: &syn::Signature,
    vis_pub: bool,
    block: &syn::Block,
    kind: &'static str,
) {
    let short = sig.ident.to_string();
    let name = match prefix {
        Some(p) => format!("{}::{}", p, short),
        None => short.clone(),
    };
    for (cn, cl, body) in named_closures(block) {
        let qual = format!("{}::{}", name, cn);
        defs.push(Def {
            id: format!("rust:{}#{}", file, qual),
            name: qual,
            short: cn,
            file: file.to_string(),
            line: cl,
            kind: "closure",
            exported: false,
            loc: 1 + spans(&body).1.saturating_sub(spans(&body).0),
            module: module.to_string(),
            calls: closure_calls(&body),
        });
    }
    defs.push(Def {
        id: format!("rust:{}#{}", file, name),
        name: name.clone(),
        short,
        file: file.to_string(),
        line: sig.ident.span().start().line,
        kind,
        exported: vis_pub,
        loc: span_loc(block),
        module: module.to_string(),
        calls: body_calls(block),
    });
}

fn walk_items(defs: &mut Vec<Def>, file: &str, module: &str, items: &[syn::Item]) {
    for item in items {
        match item {
            syn::Item::Fn(f) => {
                let vis = matches!(f.vis, syn::Visibility::Public(_));
                push_fn(defs, file, module, None, &f.sig, vis, &f.block, "function");
            }
            syn::Item::Impl(im) => {
                let ty = type_name(&im.self_ty);
                for it in &im.items {
                    if let syn::ImplItem::Fn(m) = it {
                        let vis = matches!(m.vis, syn::Visibility::Public(_)) || im.trait_.is_some();
                        push_fn(
                            defs,
                            file,
                            module,
                            Some(&ty),
                            &m.sig,
                            vis,
                            &m.block,
                            "method",
                        );
                    }
                }
            }
            syn::Item::Trait(tr) => {
                let ty = tr.ident.to_string();
                for it in &tr.items {
                    if let syn::TraitItem::Fn(m) = it {
                        if let Some(b) = &m.default {
                            push_fn(defs, file, module, Some(&ty), &m.sig, true, b, "method");
                        }
                    }
                }
            }
            syn::Item::Mod(m) => {
                if let Some((_, inner)) = &m.content {
                    let sub = format!("{}::{}", module, m.ident);
                    walk_items(defs, file, &sub, inner);
                }
            }
            _ => {}
        }
    }
}

fn main() {
    let root = std::env::args().nth(1).unwrap();
    let out = std::env::args().nth(2).unwrap();
    let files: Vec<String> = std::env::args().skip(3).collect();

    let mut defs: Vec<Def> = Vec::new();
    let mut failed: Vec<String> = Vec::new();
    for rel in &files {
        let abs = format!("{}/{}", root, rel);
        let src = std::fs::read_to_string(&abs).unwrap();
        match syn::parse_file(&src) {
            Ok(f) => {
                let module = rel
                    .rsplit('/')
                    .next()
                    .unwrap()
                    .trim_end_matches(".rs")
                    .to_string();
                walk_items(&mut defs, rel, &module, &f.items);
            }
            Err(e) => failed.push(format!("{}: {}", rel, e)),
        }
    }

    // Name index: both the qualified name (Type::method) and the bare name.
    let mut by_name: BTreeMap<String, Vec<usize>> = BTreeMap::new();
    for (i, d) in defs.iter().enumerate() {
        by_name.entry(d.name.clone()).or_default().push(i);
        if d.name != d.short {
            by_name.entry(d.short.clone()).or_default().push(i);
        }
    }

    let mut nodes: Vec<serde_json::Value> = Vec::new();
    let mut unresolved: BTreeSet<String> = BTreeSet::new();
    let mut edges: BTreeMap<(String, String), &'static str> = BTreeMap::new();

    for d in &defs {
        nodes.push(json!({
            "id": d.id, "name": d.name, "short": d.short, "file": d.file,
            "line": d.line, "lang": "rust", "kind": d.kind,
            "exported": d.exported, "loc": d.loc, "module": d.module,
        }));
    }

    for d in &defs {
        for (callee, _line, style) in &d.calls {
            // Exact qualified hit first, then bare name.
            let cand = by_name.get(callee).or_else(|| {
                callee
                    .rsplit("::")
                    .next()
                    .and_then(|bare| by_name.get(bare))
            });
            match cand {
                Some(hits) if !hits.is_empty() => {
                    // Prefer a same-file definition when the name is ambiguous.
                    let same: Vec<&usize> =
                        hits.iter().filter(|&&i| defs[i].file == d.file).collect();
                    let (chosen, conf) = if hits.len() == 1 {
                        (vec![hits[0]], "high")
                    } else if same.len() == 1 {
                        (vec![*same[0]], "high")
                    } else {
                        (hits.clone(), if *style == "method" { "low" } else { "med" })
                    };
                    for i in chosen {
                        if defs[i].id != d.id {
                            let k = (d.id.clone(), defs[i].id.clone());
                            let e = edges.entry(k).or_insert(conf);
                            if conf == "high" {
                                *e = "high";
                            }
                        }
                    }
                }
                _ => {
                    // Keep the callee visible rather than dropping it.
                    let id = format!("rust:(unresolved)#{}", callee);
                    unresolved.insert(callee.clone());
                    edges.insert((d.id.clone(), id), "low");
                }
            }
        }
    }

    for u in &unresolved {
        nodes.push(json!({
            "id": format!("rust:(unresolved)#{}", u), "name": u, "short": u,
            "file": "(unresolved)", "line": 0, "lang": "rust",
            "kind": "unresolved", "exported": false, "loc": 0, "module": "(unresolved)",
        }));
    }

    let edges: Vec<serde_json::Value> = edges
        .into_iter()
        .map(|((s, t), c)| json!({"s": s, "t": t, "conf": c}))
        .collect();

    eprintln!(
        "rust: {} files, {} defs, {} nodes, {} edges, {} parse failures",
        files.len(),
        defs.len(),
        nodes.len(),
        edges.len(),
        failed.len()
    );
    for f in &failed {
        eprintln!("  FAIL {}", f);
    }
    std::fs::write(
        out,
        serde_json::to_string(&json!({"nodes": nodes, "edges": edges})).unwrap(),
    )
    .unwrap();
}
