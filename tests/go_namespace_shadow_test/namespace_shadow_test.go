// Copyright 2026 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Package go_namespace_shadow_test guards against flatc-generated Go declaring
// a name that shadows an imported package. TestNamespaceXRoundTrip builds and
// runs code generated for a namespace named x, a name a local could take.
// TestGeneratedCodeSuffixesEveryDeclaration requires every name declared in
// any generated function to end in an underscore, whatever the name.
package go_namespace_shadow_test

import (
	"fmt"
	"go/ast"
	"go/parser"
	"go/token"
	"io/fs"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	ShadowTest "ShadowTest"
	flatbuffers "github.com/google/flatbuffers/go"
	x "x"
)

// TestNamespaceXRoundTrip packs a fully populated ShadowTest.RootT, verifies
// the buffer and unpacks it, which goes through every generated accessor.
func TestNamespaceXRoundTrip(t *testing.T) {
	red := x.ColorRed
	want := &ShadowTest.RootT{
		Color: x.ColorBlue,
		Maybe: &red,
		Point: &x.PointT{Px: 7, Color: x.ColorGreen},
		Pair: &ShadowTest.PairT{
			Point:   &x.PointT{Px: 9, Color: x.ColorBlue},
			Color:   x.ColorRed,
			Builder: 11,
		},
		Leaf:   &x.LeafT{Id: 1, Name: "one"},
		Colors: []x.Color{x.ColorRed, x.ColorBlue},
		Points: []*x.PointT{
			{Px: 1, Color: x.ColorRed},
			{Px: 2, Color: x.ColorGreen},
		},
		Leaves: []*x.LeafT{
			{Id: 2, Name: "two"},
			{Id: 3, Name: "three"},
		},
		Kind: &x.KindT{
			Type:  x.KindLeaf,
			Value: &x.LeafT{Id: 4, Name: "four"},
		},
		Local: &ShadowTest.LocalT{
			Type:  ShadowTest.LocalLeaf,
			Value: &x.LeafT{Id: 5, Name: "five"},
		},
		ByName:      map[string]*x.LeafT{"six": {Id: 6, Name: "six"}},
		ByColor:     map[x.Color]int32{x.ColorGreen: 10},
		ColorSet:    map[x.Color]struct{}{x.ColorBlue: {}},
		Flatbuffers: "flatbuffers",
		Math:        1.5,
		Builder:     12,
	}

	b := flatbuffers.NewBuilder(0)
	ShadowTest.FinishRootBuffer(b, want.Pack(b))
	buf := b.FinishedBytes()

	if err := ShadowTest.VerifyRootAsRoot(buf, nil); err != nil {
		t.Fatalf("VerifyRootAsRoot() = %v, want nil", err)
	}

	r := ShadowTest.GetRootAsRoot(buf, 0)
	got := r.UnPack()
	if !reflect.DeepEqual(got, want) {
		t.Errorf("UnPack() = %+v, want %+v", got, want)
	}

	// UnPack reaches every accessor but these three.
	var leaf x.Leaf
	if !r.LeavesByKey(&leaf, 3) {
		t.Errorf("LeavesByKey(_, 3) = false, want true")
	} else if name := string(leaf.Name()); name != "three" {
		t.Errorf("LeavesByKey(_, 3) name = %q, want %q", name, "three")
	}

	var matched int32
	got.Local.Match(func(l *x.LeafT) { matched = l.Id })
	if matched != 5 {
		t.Errorf("Local.Match() visited Id %d, want 5", matched)
	}

	if !r.MutateColor(x.ColorGreen) || r.Color() != x.ColorGreen {
		t.Errorf("MutateColor(%v) did not update Color(), now %v", x.ColorGreen, r.Color())
	}
}

// declaredIdents returns every identifier fd declares: its receiver,
// parameters and results, and everything its body declares with :=, range,
// var, const, type or a function literal's signature. Struct field names are
// not collected: a field cannot shadow a package, and the local key/value
// struct types keep fields named k and v.
func declaredIdents(fd *ast.FuncDecl) []*ast.Ident {
	var idents []*ast.Ident
	addFields := func(list *ast.FieldList) {
		if list == nil {
			return
		}
		for _, field := range list.List {
			idents = append(idents, field.Names...)
		}
	}
	addFields(fd.Recv)
	addFields(fd.Type.Params)
	addFields(fd.Type.Results)
	ast.Inspect(fd.Body, func(n ast.Node) bool {
		switch n := n.(type) {
		case *ast.AssignStmt:
			if n.Tok != token.DEFINE {
				break
			}
			for _, lhs := range n.Lhs {
				if id, ok := lhs.(*ast.Ident); ok {
					idents = append(idents, id)
				}
			}
		case *ast.RangeStmt:
			if n.Tok != token.DEFINE {
				break
			}
			if id, ok := n.Key.(*ast.Ident); ok {
				idents = append(idents, id)
			}
			if id, ok := n.Value.(*ast.Ident); ok {
				idents = append(idents, id)
			}
		case *ast.DeclStmt:
			for _, spec := range n.Decl.(*ast.GenDecl).Specs {
				switch spec := spec.(type) {
				case *ast.ValueSpec:
					idents = append(idents, spec.Names...)
				case *ast.TypeSpec:
					idents = append(idents, spec.Name)
				}
			}
		case *ast.FuncLit:
			addFields(n.Type.Params)
			addFields(n.Type.Results)
		}
		return true
	})
	return idents
}

// TestGeneratedCodeSuffixesEveryDeclaration parses every generated Go file
// under $GOPATH/src and requires each name its functions declare to end in an
// underscore.
func TestGeneratedCodeSuffixesEveryDeclaration(t *testing.T) {
	gopath := os.Getenv("GOPATH")
	if gopath == "" {
		t.Fatal("GOPATH is not set; run this test through tests/GoTest.sh")
	}
	src := filepath.Join(gopath, "src")

	fset := token.NewFileSet()
	var violations []string
	files, funcs := 0, 0

	err := filepath.WalkDir(src, func(path string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.Type().IsRegular() || !strings.HasSuffix(path, ".go") {
			return nil
		}
		file, err := parser.ParseFile(fset, path, nil, parser.ParseComments)
		if err != nil {
			return err
		}
		if !ast.IsGenerated(file) {
			return nil
		}
		files++
		for _, decl := range file.Decls {
			fd, ok := decl.(*ast.FuncDecl)
			if !ok {
				continue
			}
			funcs++
			for _, id := range declaredIdents(fd) {
				if id.Name != "_" && !strings.HasSuffix(id.Name, "_") {
					violations = append(violations, fmt.Sprintf("%s: %s declares %q", fset.Position(id.Pos()), fd.Name.Name, id.Name))
				}
			}
		}
		return nil
	})
	if err != nil {
		t.Fatalf("walking %s: %v", src, err)
	}

	if funcs == 0 {
		t.Fatalf("checked no generated functions under %s", src)
	}
	if len(violations) > 0 {
		t.Errorf("generated declarations without a trailing underscore:\n%s", strings.Join(violations, "\n"))
	}
	t.Logf("checked %d generated files, %d functions", files, funcs)
}
