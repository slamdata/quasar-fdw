CREATE FOREIGN TABLE wide_comments
(user_id integer OPTIONS (map 'userId')
,profile_name varchar OPTIONS (map 'profile.name')
,age integer OPTIONS (map 'profile.age')
,title varchar OPTIONS (map 'profile.title')
,comment_id char(10) OPTIONS (map 'comments[*].id')
,comment_text varchar OPTIONS (map 'comments[*].text')
,comment_reply_to_profile integer OPTIONS (map 'comments[*].replyTo[0]')
,comment_reply_to_comment char(10) OPTIONS (map 'comments[*].replyTo[1]')
,comment_time date OPTIONS (map 'comments[*]."time"')
,a1user_id integer OPTIONS (map 'userId')
,a1profile_name varchar OPTIONS (map 'profile.name')
,a1age integer OPTIONS (map 'profile.age')
,a1title varchar OPTIONS (map 'profile.title')
,a1comment_id char(10) OPTIONS (map 'comments[*].id')
,a1comment_text varchar OPTIONS (map 'comments[*].text')
,a1comment_reply_to_profile integer OPTIONS (map 'comments[*].replyTo[0]')
,a1comment_reply_to_comment char(10) OPTIONS (map 'comments[*].replyTo[1]')
,a1comment_time date OPTIONS (map 'comments[*]."time"')
,a2user_id integer OPTIONS (map 'userId')
,a2profile_name varchar OPTIONS (map 'profile.name')
,a2age integer OPTIONS (map 'profile.age')
,a2title varchar OPTIONS (map 'profile.title')
,a2comment_id char(10) OPTIONS (map 'comments[*].id')
,a2comment_text varchar OPTIONS (map 'comments[*].text')
,a2comment_reply_to_profile integer OPTIONS (map 'comments[*].replyTo[0]')
,a2comment_reply_to_comment char(10) OPTIONS (map 'comments[*].replyTo[1]')
,a2comment_time date OPTIONS (map 'comments[*]."time"')
,a3user_id integer OPTIONS (map 'userId')
,a3profile_name varchar OPTIONS (map 'profile.name')
,a3age integer OPTIONS (map 'profile.age')
,a3title varchar OPTIONS (map 'profile.title')
,a3comment_id char(10) OPTIONS (map 'comments[*].id')
,a3comment_text varchar OPTIONS (map 'comments[*].text')
,a3comment_reply_to_profile integer OPTIONS (map 'comments[*].replyTo[0]')
,a3comment_reply_to_comment char(10) OPTIONS (map 'comments[*].replyTo[1]')
,a3comment_time date OPTIONS (map 'comments[*]."time"')
,a4user_id integer OPTIONS (map 'userId')
,a4profile_name varchar OPTIONS (map 'profile.name')
,a4age integer OPTIONS (map 'profile.age')
,a4title varchar OPTIONS (map 'profile.title')
,a4comment_id char(10) OPTIONS (map 'comments[*].id')
,a4comment_text varchar OPTIONS (map 'comments[*].text')
,a4comment_reply_to_profile integer OPTIONS (map 'comments[*].replyTo[0]')
,a4comment_reply_to_comment char(10) OPTIONS (map 'comments[*].replyTo[1]')
,a4comment_time varchar OPTIONS (map 'comments[*].time')
,missing_space varchar OPTIONS (map 'profile.spaced field')
,missing_expand varchar OPTIONS (map 'missing[*]'))
SERVER quasar_root OPTIONS (table 'local/quasar/user_comments');
/* Now select from this wide table */
SELECT * FROM wide_comments;
