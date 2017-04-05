--jsonpath io

select ''::jsonpath;
select '$'::jsonpath;
select 'strict $'::jsonpath;
select 'lax $'::jsonpath;
select '$.a'::jsonpath;
select '$.a.v'::jsonpath;
select '$.a.*'::jsonpath;
select '$.*[*]'::jsonpath;
select '$.a[*]'::jsonpath;
select '$.a[*][*]'::jsonpath;
select '$[*]'::jsonpath;
select '$[0]'::jsonpath;
select '$[*][0]'::jsonpath;
select '$[*].a'::jsonpath;
select '$[*][0].a.b'::jsonpath;
select '$.a.**.b'::jsonpath;
select '$.a.**{2}.b'::jsonpath;
select '$.a.**{2 to 2}.b'::jsonpath;
select '$.a.**{2 to 5}.b'::jsonpath;
select '$.a.**{0 to 5}.b'::jsonpath;
select '$.a.**{5 to last}.b'::jsonpath;
select '$+1'::jsonpath;
select '$-1'::jsonpath;
select '$--+1'::jsonpath;
select '$.a/+-1'::jsonpath;

select '"\b\f\r\n\t\v\"\''\\"'::jsonpath;
select '''\b\f\r\n\t\v\"\''\\'''::jsonpath;
select '"\x50\u0067\u{53}\u{051}\u{00004C}"'::jsonpath;
select '''\x50\u0067\u{53}\u{051}\u{00004C}'''::jsonpath;
select '$.foo\x50\u0067\u{53}\u{051}\u{00004C}\t\"bar'::jsonpath;

select '$.g ? ($.a == 1)'::jsonpath;
select '$.g ? (@ == 1)'::jsonpath;
select '$.g ? (.a == 1)'::jsonpath;
select '$.g ? (@.a == 1)'::jsonpath;
select '$.g ? (@.a == 1 || @.a == 4)'::jsonpath;
select '$.g ? (@.a == 1 && @.a == 4)'::jsonpath;
select '$.g ? (@.a == 1 || @.a == 4 && @.b == 7)'::jsonpath;
select '$.g ? (@.a == 1 || !(@.a == 4) && @.b == 7)'::jsonpath;
select '$.g ? (@.a == 1 || !(@.x >= 123 || @.a == 4) && @.b == 7)'::jsonpath;
select '$.g ? (.x >= @[*]?(@.a > "abc"))'::jsonpath;
select '$.g ? ((@.x >= 123 || @.a == 4) is unknown)'::jsonpath;
select '$.g ? (exists (.x))'::jsonpath;
select '$.g ? (exists (@.x ? (@ == 14)))'::jsonpath;
select '$.g ? (exists (.x ? (@ == 14)))'::jsonpath;
select '$.g ? ((@.x >= 123 || @.a == 4) && exists (.x ? (@ == 14)))'::jsonpath;
select '$.g ? (+@.x >= +-(+@.a + 2))'::jsonpath;

select '$a'::jsonpath;
select '$a.b'::jsonpath;
select '$a[*]'::jsonpath;
select '$.g ? (@.zip == $zip)'::jsonpath;
select '$.a[1,2, 3 to 16]'::jsonpath;
select '$.a[$a + 1, ($b[*]) to -(@[0] * 2)]'::jsonpath;
select '$.a[$.a.size() - 3]'::jsonpath;
select 'last'::jsonpath;
select '"last"'::jsonpath;
select '$.last'::jsonpath;
select '$ ? (last > 0)'::jsonpath;
select '$[last]'::jsonpath;
select '$[@ ? (last > 0)]'::jsonpath;

select 'null.type()'::jsonpath;
select '1.type()'::jsonpath;
select '"aaa".type()'::jsonpath;
select 'true.type()'::jsonpath;
select '$.datetime()'::jsonpath;
select '$.datetime("datetime template")'::jsonpath;

select '$ ? (@ starts with "abc")'::jsonpath;
select '$ ? (@ starts with $var)'::jsonpath;

select '$ ? (@ like_regex "(invalid pattern")'::jsonpath;
select '$ ? (@ like_regex "pattern")'::jsonpath;
select '$ ? (@ like_regex "pattern" flag "")'::jsonpath;
select '$ ? (@ like_regex "pattern" flag "i")'::jsonpath;
select '$ ? (@ like_regex "pattern" flag "is")'::jsonpath;
select '$ ? (@ like_regex "pattern" flag "isim")'::jsonpath;
select '$ ? (@ like_regex "pattern" flag "xsms")'::jsonpath;
select '$ ? (@ like_regex "pattern" flag "a")'::jsonpath;

select '$ < 1'::jsonpath;
select '($ < 1) || $.a.b <= $x'::jsonpath;
select '@ + 1'::jsonpath;

select '($).a.b'::jsonpath;
select '($.a.b).c.d'::jsonpath;
select '($.a.b + -$.x.y).c.d'::jsonpath;
select '(-+$.a.b).c.d'::jsonpath;
select '1 + ($.a.b + 2).c.d'::jsonpath;
select '1 + ($.a.b > 2).c.d'::jsonpath;
select '($)'::jsonpath;
select '(($))'::jsonpath;
select '((($ + 1)).a + ((2)).b ? ((((@ > 1)) || (exists(@.c)))))'::jsonpath;

select '1, 2 + 3, $.a[*] + 5'::jsonpath;
select '(1, 2, $.a)'::jsonpath;
select '(1, 2, $.a).a[*]'::jsonpath;
select '(1, 2, $.a) == 5'::jsonpath;
select '$[(1, 2, $.a) to (3, 4)]'::jsonpath;
select '$[(1, (2, $.a)), 3, (4, 5)]'::jsonpath;

select '$ ? (@.a < 1)'::jsonpath;
select '$ ? (@.a < -1)'::jsonpath;
select '$ ? (@.a < +1)'::jsonpath;
select '$ ? (@.a < .1)'::jsonpath;
select '$ ? (@.a < -.1)'::jsonpath;
select '$ ? (@.a < +.1)'::jsonpath;
select '$ ? (@.a < 0.1)'::jsonpath;
select '$ ? (@.a < -0.1)'::jsonpath;
select '$ ? (@.a < +0.1)'::jsonpath;
select '$ ? (@.a < 10.1)'::jsonpath;
select '$ ? (@.a < -10.1)'::jsonpath;
select '$ ? (@.a < +10.1)'::jsonpath;
select '$ ? (@.a < 1e1)'::jsonpath;
select '$ ? (@.a < -1e1)'::jsonpath;
select '$ ? (@.a < +1e1)'::jsonpath;
select '$ ? (@.a < .1e1)'::jsonpath;
select '$ ? (@.a < -.1e1)'::jsonpath;
select '$ ? (@.a < +.1e1)'::jsonpath;
select '$ ? (@.a < 0.1e1)'::jsonpath;
select '$ ? (@.a < -0.1e1)'::jsonpath;
select '$ ? (@.a < +0.1e1)'::jsonpath;
select '$ ? (@.a < 10.1e1)'::jsonpath;
select '$ ? (@.a < -10.1e1)'::jsonpath;
select '$ ? (@.a < +10.1e1)'::jsonpath;
select '$ ? (@.a < 1e-1)'::jsonpath;
select '$ ? (@.a < -1e-1)'::jsonpath;
select '$ ? (@.a < +1e-1)'::jsonpath;
select '$ ? (@.a < .1e-1)'::jsonpath;
select '$ ? (@.a < -.1e-1)'::jsonpath;
select '$ ? (@.a < +.1e-1)'::jsonpath;
select '$ ? (@.a < 0.1e-1)'::jsonpath;
select '$ ? (@.a < -0.1e-1)'::jsonpath;
select '$ ? (@.a < +0.1e-1)'::jsonpath;
select '$ ? (@.a < 10.1e-1)'::jsonpath;
select '$ ? (@.a < -10.1e-1)'::jsonpath;
select '$ ? (@.a < +10.1e-1)'::jsonpath;
select '$ ? (@.a < 1e+1)'::jsonpath;
select '$ ? (@.a < -1e+1)'::jsonpath;
select '$ ? (@.a < +1e+1)'::jsonpath;
select '$ ? (@.a < .1e+1)'::jsonpath;
select '$ ? (@.a < -.1e+1)'::jsonpath;
select '$ ? (@.a < +.1e+1)'::jsonpath;
select '$ ? (@.a < 0.1e+1)'::jsonpath;
select '$ ? (@.a < -0.1e+1)'::jsonpath;
select '$ ? (@.a < +0.1e+1)'::jsonpath;
select '$ ? (@.a < 10.1e+1)'::jsonpath;
select '$ ? (@.a < -10.1e+1)'::jsonpath;
select '$ ? (@.a < +10.1e+1)'::jsonpath;
