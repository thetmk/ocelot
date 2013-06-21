#include "ocelot.h"
#include "db.h"
#include "misc_functions.h"
#include <string>
#include <iostream>
#include <queue>
#include <unistd.h>
#include <time.h>
#include <boost/thread/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/locks.hpp>
#include <boost/lexical_cast.hpp>

#define DB_LOCK_TIMEOUT 50

mysql::mysql(std::string mysql_db, std::string mysql_host, std::string username, std::string password) {
        if(!conn.connect(mysql_db.c_str(), mysql_host.c_str(), username.c_str(), password.c_str(), 0)) {
                std::cout << "Could not connect to MySQL" << std::endl;
                return;
        }

	db = mysql_db, server = mysql_host, db_user = username, pw = password;
	u_active = false; t_active = false; p_active = false; s_active = false; tok_active = false; hist_active = false;
		/*
		time_t now;
		time(&now);
		
		std::cout << ctime (&now) << " Connected to MySQL" << std::endl;
        */
		time_t rawtime;
		struct tm * timeinfo;
		char buffer [80];

		time ( &rawtime );
		timeinfo = localtime ( &rawtime );

		strftime (buffer,80,"%Y-%m-%d %X Connected to MySQL",timeinfo);
		std::cout << buffer << std::endl;
		
		update_user_buffer = "";
        update_torrent_buffer = "";
        update_peer_buffer = "";
        update_snatch_buffer = "";


        logger_ptr = logger::get_instance();
}

void mysql::load_site_options(site_options_t &site_options) {
        mysqlpp::Query query = conn.query("SELECT FreeLeech FROM site_options;");
        site_options.freeleech = 0;
        if(mysqlpp::StoreQueryResult res = query.store()) {
                if(res.num_rows() > 0) {
                        mysqlpp::DateTime fl = res[0][0];
                        site_options.freeleech = fl;
                }
        }
}

void mysql::load_torrents(std::unordered_map<std::string, torrent> &torrents) {
        mysqlpp::Query query = conn.query("SELECT ID, info_hash, freetorrent, double_seed, Snatched FROM torrents ORDER BY ID;");
        if(mysqlpp::StoreQueryResult res = query.store()) {
                mysqlpp::String one("1"); // Hack to get around bug in mysql++3.0.0
                mysqlpp::String two("2");
                size_t num_rows = res.num_rows();
                for(size_t i = 0; i < num_rows; i++) {
                        std::string info_hash;
                        res[i][1].to_string(info_hash);

                        torrent t;
                        t.id = res[i][0];
                        if(res[i][2].compare(one) == 0) {
                                t.free_torrent = FREE;
                        } else if(res[i][2].compare(two) == 0) {
                                t.free_torrent = NEUTRAL;
                        } else {
                                t.free_torrent = NORMAL;
                        }
                        if(res[i][3].compare(one) == 0) {
                            t.double_seed = true;
                        } else {
                            t.double_seed = false;
                        }

                        t.balance = 0;
                        t.completed = res[i][4];
                        t.last_selected_seeder = "";
                        torrents[info_hash] = t;
                }
        }
}

void mysql::load_users(std::unordered_map<std::string, user> &users) {
        mysqlpp::Query query = conn.query("SELECT ID, can_leech, torrent_pass, personal_freeleech, PermissionID FROM users_main WHERE Enabled='1';");
        if(mysqlpp::StoreQueryResult res = query.store()) {
                size_t num_rows = res.num_rows();
                for(size_t i = 0; i < num_rows; i++) {
                        std::string passkey;
                        res[i][2].to_string(passkey);

                        user u;
                        u.id = res[i][0];
                        u.can_leech = res[i][1];
                        mysqlpp::DateTime dt = res[i][3];
                        u.pfl = dt;
                        u.pmid = res[i][4];
                        users[passkey] = u;
                }
        }
}

void mysql::load_tokens(std::unordered_map<std::string, torrent> &torrents) {
        mysqlpp::Query query = conn.query("SELECT us.UserID, us.FreeLeech, us.DoubleSeed, t.info_hash FROM users_slots AS us JOIN torrents AS t ON t.ID = us.TorrentID;");
        if (mysqlpp::StoreQueryResult res = query.store()) {
                size_t num_rows = res.num_rows();
                for (size_t i = 0; i < num_rows; i++) {
                        std::string info_hash;
                        res[i][3].to_string(info_hash);
                        std::unordered_map<std::string, torrent>::iterator it = torrents.find(info_hash);
                        if (it != torrents.end()) {
                                mysqlpp::DateTime fl = res[i][1]; 
                                mysqlpp::DateTime ds = res[i][2];
                                slots_t slots;
                                slots.free_leech = fl;
                                slots.double_seed = ds;
                                
                                torrent &tor = it->second;
                                tor.tokened_users.insert(std::pair<int, slots_t>(res[i][0], slots));
                        }
                }
        }
}


void mysql::load_blacklist(std::vector<std::string> &blacklist) {
        mysqlpp::Query query = conn.query("SELECT peer_id FROM xbt_client_blacklist;");
        if(mysqlpp::StoreQueryResult res = query.store()) {
                size_t num_rows = res.num_rows();
                for(size_t i = 0; i<num_rows; i++) {
                        blacklist.push_back(res[i][0].c_str());
                }
        }
}

void mysql::record_token(std::string &record) {
        boost::mutex::scoped_lock lock(user_token_lock);
        if (update_token_buffer != "") {
                update_token_buffer += ",";
        }
        update_token_buffer += record;
}

void mysql::record_user(std::string &record) {
        boost::mutex::scoped_lock lock(user_buffer_lock);
        if(update_user_buffer != "") {
                update_user_buffer += ",";
        }
        update_user_buffer += record;
}
void mysql::record_torrent(std::string &record) {
        boost::mutex::scoped_lock lock(torrent_buffer_lock);
        if(update_torrent_buffer != "") {
                update_torrent_buffer += ",";
        }
        update_torrent_buffer += record;
}
void mysql::record_peer(std::string &record, std::string &ip, int port, std::string &peer_id, std::string &useragent) {
	// Added port to this function //Mobbo
        boost::mutex::scoped_lock lock(peer_buffer_lock);
        if(update_peer_buffer != "") {
                update_peer_buffer += ",";
        }
        mysqlpp::Query q = conn.query();
        q << record << mysqlpp::quote << ip << ',' << port << ',' << mysqlpp::quote << peer_id << ',' << mysqlpp::quote << useragent << "," << time(NULL) << ')';
	// port without qoutes since it is a int in the DB //Mobbo
        update_peer_buffer += q.str();
}

void mysql::record_peer_hist(std::string &record, std::string &peer_id, std::string &ip, int tid){
	boost::mutex::scoped_lock (peer_hist_buffer_lock);
	if (update_peer_hist_buffer != "") {
		update_peer_hist_buffer += ",";
	}
	mysqlpp::Query q = conn.query();
	q << record << ',' << mysqlpp::quote << peer_id << ',' << mysqlpp::quote << ip << ',' << tid << ',' << time(NULL) << ')';
	update_peer_hist_buffer += q.str();
}

void mysql::record_snatch(std::string &record) {
        boost::mutex::scoped_lock lock(mysql::snatch_buffer_lock);
        if(update_snatch_buffer != "") {
                update_snatch_buffer += ",";
        }
        update_snatch_buffer += record;
}

bool mysql::all_clear() {
	return (user_queue.size() == 0 && torrent_queue.size() == 0 && peer_queue.size() == 0 && snatch_queue.size() == 0 && token_queue.size() == 0 && peer_hist_queue.size() == 0);
}

void mysql::flush() {
	flush_users();
	flush_torrents();
	flush_snatches();
	flush_peers();
	flush_peer_hist();
	flush_tokens();
}

void mysql::flush_users() {
	std::string sql;
	boost::mutex::scoped_lock lock(user_buffer_lock);
	if (update_user_buffer == "") {
		return;
	}
	sql = "INSERT INTO users_main (ID, Uploaded, Downloaded, UploadedDaily, DownloadedDaily) VALUES " + update_user_buffer +
		" ON DUPLICATE KEY UPDATE Uploaded = Uploaded + VALUES(Uploaded), Downloaded = Downloaded + VALUES(Downloaded), " +
		"UploadedDaily = UploadedDaily + VALUES(UploadedDaily), DownloadedDaily = DownloadedDaily + VALUES(DownloadedDaily)";
	user_queue.push(sql);
	update_user_buffer.clear();
	if (user_queue.size() == 1 && u_active == false) {
		boost::thread thread(&mysql::do_flush_users, this);
	}
}

void mysql::flush_torrents() {
	std::string sql;
	boost::mutex::scoped_lock lock(torrent_buffer_lock);
	if (update_torrent_buffer == "") {
		return;
	}
	sql = "INSERT INTO torrents (ID,Seeders,Leechers,Snatched,Balance) VALUES " + update_torrent_buffer +
		" ON DUPLICATE KEY UPDATE Seeders=VALUES(Seeders), Leechers=VALUES(Leechers), " +
		"Snatched=Snatched+VALUES(Snatched), Balance=VALUES(Balance), last_action = " +
		"IF(VALUES(Seeders) > 0, NOW(), last_action)";
	torrent_queue.push(sql);
	update_torrent_buffer.clear();
	sql.clear();
	sql = "DELETE FROM torrents WHERE info_hash = ''";
	torrent_queue.push(sql);
	if (torrent_queue.size() == 2 && t_active == false) {
		boost::thread thread(&mysql::do_flush_torrents, this);
	}
}

void mysql::flush_snatches() {
	std::string sql;
	boost::mutex::scoped_lock lock(snatch_buffer_lock);
	if (update_snatch_buffer == "" ) {
		return;
	}
	sql = "INSERT INTO xbt_snatched (uid, fid, tstamp, IP) VALUES " + update_snatch_buffer;
	snatch_queue.push(sql);
	update_snatch_buffer.clear();
	if (snatch_queue.size() == 1 && s_active == false) {
		boost::thread thread(&mysql::do_flush_snatches, this);
	}
}

void mysql::flush_peers() {
	std::string sql;
	boost::mutex::scoped_lock lock(peer_buffer_lock);
	// because xfu inserts are slow and ram is not infinite we need to
	// limit this queue's size
	if (peer_queue.size() >= 1000) {
		peer_queue.pop();
	}
	if (update_peer_buffer == "") {
		return;
	}
	
	if (peer_queue.size() == 0) {
		sql = "SET session sql_log_bin = 0";
		peer_queue.push(sql);
		sql.clear();
	}
	
	// Added port below to record it into the DB. //Mobbo
	sql = "INSERT INTO xbt_files_users (uid,fid,active,uploaded,downloaded,upspeed,downspeed,remaining," +
		std::string("timespent,announced,ip,port,peer_id,useragent,mtime) VALUES ") + update_peer_buffer + 
				" ON DUPLICATE KEY UPDATE active=VALUES(active), uploaded=VALUES(uploaded), " +
				"downloaded=VALUES(downloaded), upspeed=VALUES(upspeed), " +
				"downspeed=VALUES(downspeed), remaining=VALUES(remaining), " +
				"timespent=VALUES(timespent), announced=VALUES(announced), " + 
				"mtime=VALUES(mtime), port=VALUES(port)";
	peer_queue.push(sql);
	update_peer_buffer.clear();
	if (peer_queue.size() == 2 && p_active == false) {
		boost::thread thread(&mysql::do_flush_peers, this);
	}
}

void mysql::flush_peer_hist() {
	std::string sql;
	boost::mutex::scoped_lock lock(peer_hist_buffer_lock);
	if (update_peer_hist_buffer == "") {
		return;
	}

	if (peer_hist_queue.size() == 0) {
		sql = "SET session sql_log_bin = 0";
		peer_hist_queue.push(sql);
		sql.clear();
	}

	sql = "INSERT IGNORE INTO xbt_peers_history (uid, downloaded, remaining, uploaded, upspeed, downspeed, timespent, peer_id, ip, fid, mtime) VALUES " + update_peer_hist_buffer;
	peer_hist_queue.push(sql);
	update_peer_hist_buffer.clear();
	if (peer_hist_queue.size() == 2 && hist_active == false) {
		boost::thread thread(&mysql::do_flush_peer_hist, this);
	}
}

void mysql::flush_tokens() {
	std::string sql;
	boost::mutex::scoped_lock lock(user_token_lock);
	if (update_token_buffer == "") {
		return;
	}
	sql = "INSERT INTO users_freeleeches (UserID, TorrentID, Downloaded, Uploaded) VALUES " + update_token_buffer +
		" ON DUPLICATE KEY UPDATE Downloaded = Downloaded + VALUES(Downloaded), Uploaded = Uploaded + VALUES(Uploaded)";
	token_queue.push(sql);
	update_token_buffer.clear();
	if (token_queue.size() == 1 && tok_active == false) {
		boost::thread(&mysql::do_flush_tokens, this);
	}
}

void mysql::do_flush_users() {
	u_active = true;
	mysqlpp::Connection c(db.c_str(), server.c_str(), db_user.c_str(), pw.c_str(), 0);
	while (user_queue.size() > 0) {
		try {
			std::string sql = user_queue.front();
			mysqlpp::Query query = c.query(sql);
			if (!query.exec()) {
				std::cout << "User flush failed (" << user_queue.size() << " remain)" << std::endl;
				sleep(3);
				continue;
			} else {
				boost::mutex::scoped_lock lock(user_buffer_lock);
				user_queue.pop();
				std::cout << "Users flushed (" << user_queue.size() << " remain)" << std::endl;
			}
		} 
		catch (const mysqlpp::BadQuery &er) {
			std::cerr << "Query error: " << er.what() << " in flush users with a qlength: " << user_queue.front().size() << " queue size: " << user_queue.size() << std::endl;
			sleep(3);
			continue;
		} catch (const mysqlpp::Exception &er) {
			std::cerr << "Query error: " << er.what() << " in flush users with a qlength: " << user_queue.front().size() <<  " queue size: " << user_queue.size() << std::endl;
			sleep(3);
			continue;
		}
	}
	u_active = false;
}

void mysql::do_flush_torrents() {
	t_active = true;
	mysqlpp::Connection c(db.c_str(), server.c_str(), db_user.c_str(), pw.c_str(), 0);
	while (torrent_queue.size() > 0) {
		try {
			std::string sql = torrent_queue.front();
			if (sql == "") {
				torrent_queue.pop();
				continue;
			}
			mysqlpp::Query query = c.query(sql);
			if (!query.exec()) {
				std::cout << "Torrent flush failed (" << torrent_queue.size() << " remain)" << std::endl;
				sleep(3);
				continue;
			} else {
				boost::mutex::scoped_lock lock(torrent_buffer_lock);
				torrent_queue.pop();
				std::cout << "Torrents flushed (" << torrent_queue.size() << " remain)" << std::endl;
			}
		}
		catch (const mysqlpp::BadQuery &er) {
			std::cerr << "Query error: " << er.what() << " in flush torrents with a qlength: " << torrent_queue.front().size() << " queue size: " << torrent_queue.size() << std::endl;
			sleep(3);
			continue;
		} catch (const mysqlpp::Exception &er) {
			std::cerr << "Query error: " << er.what() << " in flush torrents with a qlength: " << torrent_queue.front().size() << " queue size: " << torrent_queue.size() << std::endl;
			sleep(3);
			continue;
		}
	}
	t_active = false;
}

void mysql::do_flush_peers() {
	p_active = true;
	mysqlpp::Connection c(db.c_str(), server.c_str(), db_user.c_str(), pw.c_str(), 0);
	while (peer_queue.size() > 0) {
		try {
			std::string sql = peer_queue.front();
			mysqlpp::Query query = c.query(sql);
			if (!query.exec()) {
				std::cout << "Peer flush failed (" << peer_queue.size() << " remain)" << std::endl;
				sleep(3);
				continue;
			} else {
				boost::mutex::scoped_lock lock(peer_buffer_lock);
				peer_queue.pop();
				std::cout << "Peers flushed (" << peer_queue.size() << " remain)" << std::endl;
			}
		}
		catch (const mysqlpp::BadQuery &er) {
			std::cerr << "Query error: " << er.what() << " in flush peers with a qlength: " << peer_queue.front().size() << " queue size: " << peer_queue.size() << std::endl;
			sleep(3);
			continue;
		} catch (const mysqlpp::Exception &er) {
			std::cerr << "Query error: " << er.what() << " in flush peers with a qlength: " << peer_queue.front().size() << " queue size: " << peer_queue.size() << std::endl;
			sleep(3);
			continue;
		}
	}
	p_active = false;
}

void mysql::do_flush_peer_hist() {
	hist_active = true;
	mysqlpp::Connection c(db.c_str(), server.c_str(), db_user.c_str(), pw.c_str(), 0);
	while (peer_hist_queue.size() > 0) {
		try {
			std::string sql = peer_hist_queue.front();
			mysqlpp::Query query = c.query(sql);
			if (!query.exec()) {
				std::cout << "Peer history flush failed (" << peer_hist_queue.size() << " remain)" << std::endl;
				sleep(3);
				continue;
			} else {
				boost::mutex::scoped_lock lock(peer_hist_buffer_lock);
				peer_hist_queue.pop();
				std::cout << "Peer history flushed (" << peer_hist_queue.size() << " remain)" << std::endl;
			}
		}
		catch (const mysqlpp::BadQuery &er) {
			std::cerr << "Query error: " << er.what() << " in flush peer history with a qlength: " << peer_hist_queue.front().size() << " queue size: " << peer_hist_queue.size() << std::endl;
			sleep(3);
			continue;
		} catch (const mysqlpp::Exception &er) {
			std::cerr << "Query error: " << er.what() << " in flush peer history with a qlength: " << peer_hist_queue.front().size() << " queue size: " << peer_hist_queue.size() << std::endl;
		sleep(3);
		continue;
		}
	}
	hist_active = false;
}

void mysql::do_flush_snatches() {
	s_active = true;
	mysqlpp::Connection c(db.c_str(), server.c_str(), db_user.c_str(), pw.c_str(), 0);
	while (snatch_queue.size() > 0) {
		try {
			std::string sql = snatch_queue.front();
			mysqlpp::Query query = c.query(sql);
			if (!query.exec()) {
				std::cout << "Snatch flush failed (" << snatch_queue.size() << " remain)" << std::endl;
				sleep(3);
				continue;
			} else {
				boost::mutex::scoped_lock lock(snatch_buffer_lock);
				snatch_queue.pop();
				std::cout << "Snatches flushed (" << snatch_queue.size() << " remain)" << std::endl;
			}
		} 
		catch (const mysqlpp::BadQuery &er) {
			std::cerr << "Query error: " << er.what() << " in flush snatches with a qlength: " << snatch_queue.front().size() << " queue size: " << snatch_queue.size() << std::endl;
			sleep(3);
			continue;
		} catch (const mysqlpp::Exception &er) {
			std::cerr << "Query error: " << er.what() << " in flush snatches with a qlength: " << snatch_queue.front().size() << " queue size: " << snatch_queue.size() << std::endl;
			sleep(3);
			continue;
		}
	}
	s_active = false;
}

void mysql::do_flush_tokens() {
	tok_active = true;
	mysqlpp::Connection c(db.c_str(), server.c_str(), db_user.c_str(), pw.c_str(), 0);
	while (token_queue.size() > 0) {
		try {
			std::string sql = token_queue.front();
			mysqlpp::Query query = c.query(sql);
			if (!query.exec()) {
				std::cout << "Token flush failed (" << token_queue.size() << " remain)" << std::endl;
				sleep(3);
				continue;
			} else {
				boost::mutex::scoped_lock lock(user_token_lock);
				token_queue.pop();
				std::cout << "Tokens flushed (" << token_queue.size() << " remain)" << std::endl;
			}
		}
		catch (const mysqlpp::BadQuery &er) {
			std::cerr << "Query error: " << er.what() << " in flush tokens with a qlength: " << token_queue.front().size() << " queue size: " << token_queue.size() << std::endl;
			sleep(3);
			continue;
		} catch (const mysqlpp::Exception &er) {
			std::cerr << "Query error: " << er.what() << " in flush tokens with a qlength: " << token_queue.front().size() << " queue size: " << token_queue.size() << std::endl;
			sleep(3);
			continue;
		}
	}
	tok_active = false;
}
