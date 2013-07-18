from django.db import models

# Create your models here.
class Lesson(models.Model):
    name = models.CharField(max_length = 100)
    tutorial = models.TextField()
    created = models.DateField(auto_now_add = True) #Creation date set on adding
    modified = models.DateField(auto_now = True) #Modification date set on changing
    times_taken = models.IntegerField() #TODO make this a cache for query on usertakeslessons
    def __unicode__(self):
        return self.name

class Example(models.Model):
    lesson = models.ForeignKey(Lesson)
    problem = models.TextField()
    solution = models.TextField()
    created = models.DateField(auto_now_add = True) #Creation date set on adding
    modified = models.DateField(auto_now = True) #Modification date set on changing
    def __unicode__(self):
        return self.problem

class AnswerFormat(models.Model):
    name = models.CharField(max_length = 100)
    hint = models.CharField(max_length = 250)
    example = models.CharField(max_length = 250)
    regex = models.CharField(max_length = 300)
    def __unicode__(self):
        return self.name

class Question(models.Model):
    lesson = models.ForeignKey(Lesson)
    question = models.CharField(max_length = 300)
    answer = models.CharField(max_length = 200)
    answer_format = models.ForeignKey(AnswerFormat)
    calculation = models.TextField() #TODO work out how on earth this will work
    created = models.DateField(auto_now_add = True) #Creation date set on adding
    modified = models.DateField(auto_now = True) #Modification date set on changing
    times_answered = models.IntegerField() #TODO make this cache of query as above
    def __unicode__(self):
        return self.question
