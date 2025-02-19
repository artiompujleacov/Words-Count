#include <iostream>
#include <fstream>
#include <vector>
#include <pthread.h>
#include <string>
#include <map>
#include <queue>
#include <unordered_map>
#include <set>
#include <cctype>
#include <algorithm>
#include <sys/stat.h>

using namespace std;

struct FileInfo
{
    int index;
    string name;
    size_t size;

    bool operator<(const FileInfo &other) const
    {
        return size < other.size;
    }
};

struct thread_data
{
    vector<FileInfo> file_queue;
    vector<unordered_map<string, set<int>>> *temporary_map;
    unordered_map<string, set<int>> *words_ready;
    pthread_mutex_t *words_ready_mutex;
    pthread_barrier_t *barrier;
    int thread_id;
    char start_letter;
    char end_letter;
};

void writer(const unordered_map<string, set<int>> &words_ready, char start_letter, char end_letter)
{
    map<char, vector<pair<string, set<int>>>> categorized_words;
    for (const auto &[word, files] : words_ready)
    {
        if (word[0] >= start_letter && word[0] <= end_letter && isalpha(word[0]))
        {
            categorized_words[word[0]].push_back({word, files});
        }
    }

    for (auto &[letter, words] : categorized_words)
    {
        sort(words.begin(), words.end(), [](const auto &a, const auto &b)
             {
                if (a.second.size() != b.second.size()) {
                    return a.second.size() > b.second.size();
                }
                return a.first < b.first; });
    }

    for (char letter = start_letter; letter <= end_letter; ++letter)
    {
        string file_name = string(1, letter) + ".txt";
        ofstream fout(file_name);

        if (categorized_words.count(letter))
        {
            for (const auto &[word, files] : categorized_words[letter])
            {
                fout << word << ": [";

                string file_indices;
                for (int file_index : files)
                {
                    file_indices += to_string(file_index) + " ";
                }
                if (!file_indices.empty())
                    file_indices.pop_back();

                fout << file_indices << "]" << endl;
            }
        }
        fout.close();
    }
}

size_t get_file_size(const string &file_name)
{
    struct stat stat_buf;
    if (stat(file_name.c_str(), &stat_buf) == 0)
        return stat_buf.st_size;
    return 0;
}

void to_lowercase_and_filter(string &word)
{
    word.erase(remove_if(word.begin(), word.end(), [](char c)
                         { return !isalpha(c); }),
               word.end());
    transform(word.begin(), word.end(), word.begin(), ::tolower);
}

void *maper(void *arg)
{
    thread_data *args = (thread_data *)arg;
    auto &file_queue = args->file_queue;
    auto *temporary_map = args->temporary_map;
    int mapper_id = args->thread_id;
    auto *barrier = args->barrier;

    unordered_map<string, set<int>> part_map;

    while (true)
    {
        FileInfo file_info;
        pthread_mutex_lock(args->words_ready_mutex);
        if (file_queue.empty())
        {
            pthread_mutex_unlock(args->words_ready_mutex);
            break;
        }
        file_info = file_queue.back();
        file_queue.pop_back();
        pthread_mutex_unlock(args->words_ready_mutex);

        int file_index = file_info.index;
        string file_name = file_info.name;

        ifstream file(file_name);
        string word;

        while (file >> word)
        {
            to_lowercase_and_filter(word);
            if (!word.empty())
            {
                part_map[word].insert(file_index);
            }
        }
        file.close();
    }

    (*temporary_map)[mapper_id] = part_map;

    pthread_barrier_wait(barrier);
    return NULL;
}

void *reducer(void *arg)
{
    thread_data *args = (thread_data *)arg;
    auto *temporary_map = args->temporary_map;
    char start_letter = args->start_letter;
    char end_letter = args->end_letter;
    auto *words_ready = new unordered_map<string, set<int>>();
    pthread_barrier_t *barrier = args->barrier;

    pthread_barrier_wait(barrier);

    for (auto map_iter = temporary_map->begin(); map_iter != temporary_map->end(); ++map_iter)
    {
        for (auto word_iter = map_iter->begin(); word_iter != map_iter->end(); ++word_iter)
        {
            const string &word = word_iter->first;
            const set<int> &files = word_iter->second;

            if (word[0] >= start_letter && word[0] <= end_letter)
            {
                auto &target_files = (*words_ready)[word];
                target_files.insert(files.begin(), files.end());
            }
        }
    }

    writer(*words_ready, start_letter, end_letter);

    delete words_ready;
    return NULL;
}

int main(int argc, char *argv[])
{
    int num_mappers = stoi(argv[1]);
    int num_reducers = stoi(argv[2]);
    string input_file = argv[3];

    ifstream fin(input_file);
    int num_files;
    fin >> num_files;

    vector<FileInfo> files;
    for (int i = 0; i < num_files; ++i)
    {
        string file_name;
        fin >> file_name;
        size_t file_size = get_file_size(file_name);
        files.push_back({i + 1, file_name, file_size});
    }

    fin.close();

    sort(files.begin(), files.end());

    vector<vector<FileInfo>> mapper_files(num_mappers);
    for (int i = files.size() - 1; i >= 0; --i)
    {
        int mapper_id = i % num_mappers;
        mapper_files[mapper_id].push_back(files[i]);
    }

    vector<unordered_map<string, set<int>>> temporary_map(num_mappers);

    pthread_mutex_t words_ready_mutex;
    pthread_mutex_init(&words_ready_mutex, NULL);

    pthread_barrier_t barrier;
    pthread_barrier_init(&barrier, NULL, num_mappers + num_reducers);

    vector<pthread_t> threads(num_mappers + num_reducers);

    unordered_map<string, set<int>> words_ready;

    char start = 'a';
    int interval = 26 / num_reducers;
    int remainder = 26 % num_reducers;

    for (int i = 0; i < num_mappers + num_reducers; ++i)
    {
        if (i < num_mappers)
        {
            thread_data *args = new thread_data{
                mapper_files[i],
                &temporary_map,
                nullptr,
                &words_ready_mutex,
                &barrier,
                i,
                '\0',
                '\0'};
            pthread_create(&threads[i], NULL, maper, args);
        }
        else
        {
            char end = min(start + interval - 1 + (remainder > 0 ? 1 : 0), (int)'z');
            thread_data *args = new thread_data{
                {},
                &temporary_map,
                &words_ready,
                &words_ready_mutex,
                &barrier,
                i,
                start,
                end};
            pthread_create(&threads[i], NULL, reducer, args);
            start = end + 1;
        }
    }

    for (int i = 0; i < num_mappers + num_reducers; ++i)
    {
        pthread_join(threads[i], NULL);
    }

    pthread_barrier_destroy(&barrier);
    pthread_mutex_destroy(&words_ready_mutex);

    return 0;
}
