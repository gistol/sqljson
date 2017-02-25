--jsonpath io

select '$'::jsonpath;
select '$.a'::jsonpath;
select '$.a.v'::jsonpath;
select '$.a.*'::jsonpath;
select '$.*.[*]'::jsonpath;
select '$.*[*]'::jsonpath;
select '$.a.[*]'::jsonpath;
select '$.a[*]'::jsonpath;
select '$.a.[*][*]'::jsonpath;
select '$.a.[*].[*]'::jsonpath;
select '$.a[*][*]'::jsonpath;
select '$.a[*].[*]'::jsonpath;
select '$[*]'::jsonpath;
select '$[0]'::jsonpath;
select '$[*][0]'::jsonpath;
select '$[*].a'::jsonpath;
select '$[*][0].a.b'::jsonpath;

select '$.g ? (@ = 1)'::jsonpath;
select '$.g ? (a = 1)'::jsonpath;
select '$.g ? (.a = 1)'::jsonpath;
select '$.g ? (@.a = 1)'::jsonpath;
select '$.g ? (@.a = 1 || a = 4)'::jsonpath;
select '$.g ? (@.a = 1 && a = 4)'::jsonpath;
select '$.g ? (@.a = 1 || a = 4 && b = 7)'::jsonpath;
select '$.g ? (@.a = 1 || !a = 4 && b = 7)'::jsonpath;
select '$.g ? (@.a = 1 || !(x >= 123 || a = 4) && b = 7)'::jsonpath;

select '$.g ? (zip = $zip)'::jsonpath;
select '$.a.[1,2, 3 to 16]'::jsonpath;
select '$.a[1,2, 3 to 16]'::jsonpath;

select '$ ? (a < 1)'::jsonpath;
select '$ ? (a < -1)'::jsonpath;
select '$ ? (a < +1)'::jsonpath;
select '$ ? (a < .1)'::jsonpath;
select '$ ? (a < -.1)'::jsonpath;
select '$ ? (a < +.1)'::jsonpath;
select '$ ? (a < 0.1)'::jsonpath;
select '$ ? (a < -0.1)'::jsonpath;
select '$ ? (a < +0.1)'::jsonpath;
select '$ ? (a < 10.1)'::jsonpath;
select '$ ? (a < -10.1)'::jsonpath;
select '$ ? (a < +10.1)'::jsonpath;
select '$ ? (a < 1e1)'::jsonpath;
select '$ ? (a < -1e1)'::jsonpath;
select '$ ? (a < +1e1)'::jsonpath;
select '$ ? (a < .1e1)'::jsonpath;
select '$ ? (a < -.1e1)'::jsonpath;
select '$ ? (a < +.1e1)'::jsonpath;
select '$ ? (a < 0.1e1)'::jsonpath;
select '$ ? (a < -0.1e1)'::jsonpath;
select '$ ? (a < +0.1e1)'::jsonpath;
select '$ ? (a < 10.1e1)'::jsonpath;
select '$ ? (a < -10.1e1)'::jsonpath;
select '$ ? (a < +10.1e1)'::jsonpath;
select '$ ? (a < 1e-1)'::jsonpath;
select '$ ? (a < -1e-1)'::jsonpath;
select '$ ? (a < +1e-1)'::jsonpath;
select '$ ? (a < .1e-1)'::jsonpath;
select '$ ? (a < -.1e-1)'::jsonpath;
select '$ ? (a < +.1e-1)'::jsonpath;
select '$ ? (a < 0.1e-1)'::jsonpath;
select '$ ? (a < -0.1e-1)'::jsonpath;
select '$ ? (a < +0.1e-1)'::jsonpath;
select '$ ? (a < 10.1e-1)'::jsonpath;
select '$ ? (a < -10.1e-1)'::jsonpath;
select '$ ? (a < +10.1e-1)'::jsonpath;
select '$ ? (a < 1e+1)'::jsonpath;
select '$ ? (a < -1e+1)'::jsonpath;
select '$ ? (a < +1e+1)'::jsonpath;
select '$ ? (a < .1e+1)'::jsonpath;
select '$ ? (a < -.1e+1)'::jsonpath;
select '$ ? (a < +.1e+1)'::jsonpath;
select '$ ? (a < 0.1e+1)'::jsonpath;
select '$ ? (a < -0.1e+1)'::jsonpath;
select '$ ? (a < +0.1e+1)'::jsonpath;
select '$ ? (a < 10.1e+1)'::jsonpath;
select '$ ? (a < -10.1e+1)'::jsonpath;
select '$ ? (a < +10.1e+1)'::jsonpath;
