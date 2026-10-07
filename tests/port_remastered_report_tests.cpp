// The converter reports' formatting: column counts, field cleaning, sorting, deduping, the
// summary's counts. No game files needed.

#include "port_remastered_report.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {

int sFailures = 0;

void Check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

size_t Tabs(const std::string& s) {
  size_t n = 0;
  for (char c : s) {
    n += c == '\t';
  }
  return n;
}

MaterialDecision Sample(int index, const char* kindReason, int kind) {
  MaterialDecision d;
  d.cmdl = "0A1B2C3D";
  d.index = index;
  d.source = "a\tb\nc";
  d.shader = "DEADBEEF";
  d.flags = 0x1A;
  d.tag = "PBR5";
  d.kind = kind;
  d.path = "pbr";
  d.pathReason = "ok";
  d.kindReason = kindReason;
  d.emissive = 0.125;
  return d;
}

void TestMaterialRows() {
  const std::string header = MaterialReportHeader();
  const std::string row = FormatMaterialRow(Sample(7, "list", 3));
  Check(Tabs(row) == Tabs(header), "material row has one field per column");
  Check(row.find('\n') == std::string::npos, "a newline in a field is removed");
  Check(row.find("a b c") != std::string::npos, "tab/newline in a field become spaces");
  Check(row.find("\t007\t") != std::string::npos, "material index is zero padded");
  Check(row.find("0x1A") != std::string::npos, "flags are hex");
  Check(row.find("\t0.125\t") != std::string::npos, "numbers are short");
  Check(row.find("\t\t") == std::string::npos, "an empty field is a dash");
}

void TestEffectRows() {
  EffectReportRow r;
  r.genp = "00112233-4455-6677-8899-aabbccddeeff";
  r.retail = 0x1234ABCD;
  r.result = "imported";
  r.method = "name";
  r.dropped = 2;
  r.droppedList = {"ABCD: no retail form", "EFGH: x"};
  const std::string row = FormatEffectRow(r);
  Check(Tabs(row) == Tabs(EffectReportHeader()), "effect row has one field per column");
  Check(row.find("1234ABCD") != std::string::npos, "retail id in hex");
  Check(row.find("ABCD: no retail form;EFGH: x") != std::string::npos, "dropped list joined with ';'");
  EffectReportRow u;
  u.genp = "x";
  u.result = "unpaired";
  u.method = "none";
  Check(FormatEffectRow(u).find("\t-\t") != std::string::npos, "unpaired has no retail id");
}

void TestJoinAndSummary() {
  std::vector<std::string> rows = {FormatMaterialRow(Sample(2, "list", 3)), FormatMaterialRow(Sample(1, "default", 0)),
                                   FormatMaterialRow(Sample(2, "list", 3))};
  const std::string text = JoinReport(MaterialReportHeader(), rows);
  const std::vector<std::string> back = ReportRows(text);
  Check(back.size() == 2, "duplicates are dropped");
  Check(back.size() == 2 && back[0] < back[1], "rows are sorted");
  Check(JoinReport(MaterialReportHeader(), {rows[1], rows[0]}) == JoinReport(MaterialReportHeader(), {rows[0], rows[1]}),
        "the order the rows arrive in does not matter");
  EffectReportRow a;
  a.genp = "g1";
  a.result = "failed";
  a.method = "name";
  a.reason = "no texture: abc";
  EffectReportRow b = a;
  b.genp = "g2";
  b.result = "imported";
  b.dropped = 3;
  const std::string effects = JoinReport(EffectReportHeader(), {FormatEffectRow(a), FormatEffectRow(b)});
  const std::string summary = SummarizeReports(text, effects);
  Check(summary.find("materials: 2") != std::string::npos, "summary counts materials");
  Check(summary.find("effects: 2") != std::string::npos, "summary counts effects");
  Check(summary.find("imported via name") != std::string::npos, "summary counts results by method");
  Check(summary.find("no texture") != std::string::npos, "summary counts failure reasons");
  Check(SummarizeReports("", "").find("materials: 0") != std::string::npos, "an empty report summarises");
}

}  // namespace

int main() {
  TestMaterialRows();
  TestEffectRows();
  TestJoinAndSummary();
  if (sFailures == 0) {
    std::puts("port_remastered_report_tests: ok");
  }
  return sFailures == 0 ? 0 : 1;
}
