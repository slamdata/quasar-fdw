CREATE FOREIGN TABLE zips(city varchar, pop integer, state char(2))
       SERVER quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE smallzips(city varchar, pop integer, state char(2))
       SERVER quasar OPTIONS (table 'smallZips');
CREATE FOREIGN TABLE zipsloc(loc float[2])
       SERVER quasar OPTIONS (table 'smallZips');
CREATE FOREIGN TABLE zipsjson(city varchar, pop integer, loc json, locb jsonb OPTIONS (map 'loc'))
       SERVER quasar OPTIONS (table 'smallZips');
CREATE FOREIGN TABLE nested(a varchar OPTIONS (map 'topObj.midObj.botObj.a'),
                            b varchar OPTIONS (map 'topObj.midObj.botObj.b'),
                            c varchar OPTIONS (map 'topObj.midObj.botObj.c'))
       SERVER quasar OPTIONS (table 'nested');
CREATE FOREIGN TABLE zips_missing(city varchar, missing varchar)
       SERVER quasar OPTIONS (table 'smallZips');
CREATE FOREIGN TABLE zips_bad(loc float) -- BAD Field type
       SERVER quasar OPTIONS (table 'smallZips');
CREATE FOREIGN TABLE zips_convert(pop integer, loc0 float OPTIONS (map 'loc[0]'))
       SERVER quasar OPTIONS (table 'smallZips');
CREATE FOREIGN TABLE commits
       (ts timestamp OPTIONS (map 'commit.author.`date`', nopushdown 'true')
       ,sha varchar
       ,author_name varchar OPTIONS (map 'commit.author.name')
       ,author_email varchar OPTIONS (map 'commit.author.email')
       ,url varchar
       ,comment_count integer OPTIONS (map 'commit.comment_count'))
       SERVER quasar OPTIONS (table 'slamengine_commits');
CREATE FOREIGN TABLE commits_timestamps
       (ts timestamp OPTIONS (map 'commit.author.date')
       ,tstz timestamptz OPTIONS (map 'commit.author.date')
       ,sha varchar)
       SERVER quasar OPTIONS (table 'slamengine_commits_dates');
CREATE FOREIGN TABLE olympics
       (yr char(4) OPTIONS (map 'year')
       ,yearint integer OPTIONS (map 'year')
       ,yearfloat float OPTIONS (map 'year')
       ,city varchar
       ,sport varchar
       ,discipline varchar
       ,country varchar
       ,event varchar
       ,gender char(1)
       ,type varchar)
       SERVER quasar OPTIONS (table 'olympics');
CREATE FOREIGN TABLE nested_expansion (vals integer OPTIONS (map 'topArr[*].botArr[*]'), topObj json OPTIONS (map 'topObj'))
       SERVER quasar OPTIONS (table 'nested');
CREATE FOREIGN TABLE user_comments
       (user_id integer OPTIONS (map 'userId')
       ,profile_name varchar OPTIONS (map 'profile.name')
       ,age integer OPTIONS (map 'profile.age')
       ,title varchar OPTIONS (map 'profile.title')
       ,comment_id char(10) OPTIONS (map 'comments[*].id')
       ,comment_text varchar OPTIONS (map 'comments[*].text')
       ,comment_reply_to_profile integer OPTIONS (map 'comments[*].replyTo[0]')
       ,comment_reply_to_comment char(10) OPTIONS (map 'comments[*].replyTo[1]')
       ,comment_time date OPTIONS (map 'comments[*].`time`'))
       SERVER quasar OPTIONS (table 'user_comments');
CREATE FOREIGN TABLE zips_re(city varchar OPTIONS (join_rowcount_estimate '1')
                            ,pop integer
                            ,state char(2) OPTIONS (join_rowcount_estimate '500'))
       SERVER quasar OPTIONS (table 'zips'
                             ,use_remote_estimate 'true');
CREATE FOREIGN TABLE smallzips_re(city varchar, pop integer, state char(2))
       SERVER quasar OPTIONS (table 'smallZips'
                             ,use_remote_estimate 'true');
CREATE FOREIGN TABLE test_times(t time)
       SERVER quasar OPTIONS (table 'testtime_doesnt_exist');
CREATE FOREIGN TABLE test_intervals(i interval)
       SERVER quasar OPTIONS (table 'testintervals_doesnt_exist');
CREATE FOREIGN TABLE zips_badtype(city int, state char(1), pop date)
       SERVER quasar OPTIONS (table 'zips');
CREATE FOREIGN TABLE weird_fields(
               spaced varchar OPTIONS (map 'topObj.some field'),
               underscored varchar OPTIONS (map '__underscored'),
               toparray json OPTIONS (map 'topArr[*]'))
       SERVER quasar OPTIONS (table 'nested');
